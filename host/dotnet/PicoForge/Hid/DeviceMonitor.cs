using Microsoft.Extensions.Logging.Abstractions;

namespace PicoForge;

/// <summary>
/// One board's monitor. Commands throw instead of returning a flag: see <c>UseApiErrors</c>. <c>cancel</c> stops only
/// the wait for the board (an aborted request); a command that reached the board always runs to completion.
/// </summary>
internal interface IDeviceMonitor : IDisposable, IAsyncDisposable
{
    StatusResponse Snapshot { get; }
    void Start();
    /// <summary>Claims idle, resolved command ownership without waiting; refuses all subsequent commands.</summary>
    bool TryRetire();
    /// <summary>Blocks new writes without interrupting an accepted command or losing its result.</summary>
    void SetQuarantined(bool value);
    CommandResyncResponse ResyncCommand(bool confirm, CancellationToken cancel = default);
    void SetLed(bool on, CancellationToken cancel = default);
    MscStatusResponse SetMsc(bool enabled, bool force = false, CancellationToken cancel = default);
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false,
        CancellationToken cancel = default);
    void Reboot(CancellationToken cancel = default);
    void SetSelfTest(string action, uint sizeMib, CancellationToken cancel = default);
    WirelessResponse SetWireless(WirelessRequest settings, CancellationToken cancel = default);
    IReadOnlyList<WirelessNetworkResponse> ScanWireless(CancellationToken cancel = default);
    WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null,
        CancellationToken cancel = default);
    void CancelWirelessTest(CancellationToken cancel = default);
    byte[] TcpCliAuthProof(byte[] nonce, byte[] uid, CancellationToken cancel = default);
}

/// <summary>The board is running another command; nothing was sent, so retrying is safe.</summary>
internal sealed class BoardBusyException()
    : BoardStateException("Board is busy with another command; retry when it finishes");

/// <summary>Serialized HID operations used by <see cref="DeviceMonitor"/>; allows tests to block a write.</summary>
internal interface IHidCommands
{
    bool CommandOutcomeUncertain { get; }
    ControlCommandReference? UncertainCommand { get; }
    WirelessOperationReference? UncertainWirelessOperation { get; }
    InputReport? ReadInputReport(int timeoutMilliseconds = 250);
    void SetLed(bool on);
    void SetMsc(bool enabled, bool force = false);
    BoardConfigurationResponse ReadConfiguration();
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false);
    void Reboot();
    void SetSelfTest(string action, uint sizeMib);
    WirelessStatusResponse ReadWireless();
    byte[] TcpCliAuthProof(byte[] nonce, byte[] uid);
    CommandResyncResult? ReconcileOutcome(ControlCommandReference? command,
        WirelessOperationReference? wirelessOperation);
    WirelessStatusResponse SetWireless(WirelessRequest settings);
    IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null);
    WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
        Action<string>? onProgress = null, string? ssidHex = null, CancellationToken cancel = default);
    void CancelWirelessTest();
}

/// <summary>An opened board handle: identity plus <see cref="IHidCommands"/>; lets tests replace HID.</summary>
internal interface IHidClient : IHidCommands, IDisposable
{
    HardwareInfoResponse ReadInfo();
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private const string UncertainMessage = "A prior HID command outcome is uncertain; further writes are blocked";
    // An API command waits this long for the board, then gets a 409; Wi-Fi scan and test hold it up to 15 s and 35 s.
    private static readonly TimeSpan BusyWait = TimeSpan.FromSeconds(2);
    private readonly Func<IHidClient> createClient;
    private readonly ILogger<DeviceMonitor> logger;
    private readonly TimeProvider time;
    private readonly object gate = new();
    private readonly object lifecycleGate = new();
    private Task? disposal;
    private int stopping;
    private int quarantined;
    private Exception? cleanupError;
    // Handle operations always acquire deviceGate before the snapshot gate. Not reentrant.
    private readonly SemaphoreSlim deviceGate = new(1, 1);
    private IHidCommands? activeClient;
    // TimeProvider timestamps of the last report per section, guarded by gate; 0 means none since connecting.
    private long telemetryReceipt;
    private long storageReceipt;
    private long selfTestReceipt;
    private long wirelessReceipt;
    private ControlCommandReference? uncertainCommand;
    private WirelessOperationReference? uncertainWirelessOperation;
    private long mscReconnectDeadline;
    private bool? mscReconnectEnabled;
    // Set while a Wi-Fi test owns deviceGate, guarded by gate, so a cancel can reach it without waiting.
    private CancellationTokenSource? wirelessTestCancel;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID");

    public DeviceMonitor() : this((string?)null, null) { }

    public DeviceMonitor(string? serialNumber, string? devicePath, ILogger<DeviceMonitor>? logger = null)
        : this(() => new PicoForgeHid(serialNumber, devicePath), logger ?? NullLogger<DeviceMonitor>.Instance) { }

    internal DeviceMonitor(Func<IHidClient> createClient, ILogger<DeviceMonitor>? logger = null,
        TimeProvider? time = null)
    {
        this.createClient = createClient;
        this.logger = logger ?? NullLogger<DeviceMonitor>.Instance;
        this.time = time ?? TimeProvider.System;
    }

    public void Start()
    {
        lock (lifecycleGate)
        {
            if (Volatile.Read(ref stopping) != 0) throw new BoardStateException("Board monitor is shutting down");
            StartAsync(CancellationToken.None).GetAwaiter().GetResult();
        }
    }

    public bool TryRetire()
    {
        if (!deviceGate.Wait(0)) return false;
        try
        {
            if (Snapshot.CommandOutcomeUncertain) return false;
            Interlocked.Exchange(ref stopping, 1);
            return true;
        }
        finally { deviceGate.Release(); }
    }

    public void SetQuarantined(bool value) => Volatile.Write(ref quarantined, value ? 1 : 0);

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        Interlocked.Exchange(ref stopping, 1);
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
    }

    public ValueTask DisposeAsync()
    {
        lock (lifecycleGate)
        {
            Interlocked.Exchange(ref stopping, 1);
            disposal ??= Task.Run(StopAndDisposeAsync);
            return new(disposal);
        }
    }

    private async Task StopAndDisposeAsync()
    {
        try
        {
            await StopAsync(CancellationToken.None).ConfigureAwait(false);
            if (ExecuteTask is { } worker)
            {
                try { await worker.ConfigureAwait(false); }
                catch (OperationCanceledException) when (Volatile.Read(ref stopping) != 0) { }
            }
            using (Owner())
            lock (gate) activeClient = null;
            if (cleanupError is not null)
                throw new IOException("HID cleanup failed; restart the dashboard before reconnecting", cleanupError);
        }
        finally { base.Dispose(); }
    }

    public override void Dispose() => DisposeAsync().AsTask().GetAwaiter().GetResult();

    /// <summary>Holds <see cref="deviceGate"/> until disposed.</summary>
    private readonly struct DeviceLease(SemaphoreSlim gate) : IDisposable
    {
        public void Dispose() => gate.Release();
    }

    /// <summary>
    /// API commands wait at most <see cref="BusyWait"/>; a busy board is a 409 because nothing was sent.
    /// </summary>
    private DeviceLease Device(CancellationToken cancel, bool readOnly = false)
    {
        if (!deviceGate.Wait(BusyWait, cancel)) throw new BoardBusyException();
        if (Volatile.Read(ref stopping) != 0)
        {
            deviceGate.Release();
            throw new BoardStateException("Board monitor is shutting down");
        }
        if (!readOnly && Volatile.Read(ref quarantined) != 0)
        {
            deviceGate.Release();
            throw new BoardStateException("Board identity is ambiguous; rescan after removing duplicate identities");
        }
        return new(deviceGate);
    }

    /// <summary>The monitor loop owns the device, so it waits without a limit.</summary>
    private DeviceLease Owner()
    {
        deviceGate.Wait();
        return new(deviceGate);
    }

    private async ValueTask<DeviceLease> OwnerAsync(CancellationToken cancel)
    {
        await deviceGate.WaitAsync(cancel);
        return new(deviceGate);
    }

    /// <summary>The latest reports, with any section older than its freshness limit hidden.</summary>
    public StatusResponse Snapshot
    {
        get { lock (gate) return Fresh(); }
    }

    public CommandResyncResponse ResyncCommand(bool confirm, CancellationToken cancel = default)
    {
        if (!confirm) throw new ApiInputException("Explicit confirm: true is required");
        using (Device(cancel, readOnly: true))
        {
            if (!Snapshot.CommandOutcomeUncertain) return new(false);
            var client = Client(requireConfiguration: false);
            CommandResyncResult? result;
            try
            {
                result = client.ReconcileOutcome(uncertainCommand, uncertainWirelessOperation);
            }
            catch (Exception error) when (error is IOException or InvalidDataException or TimeoutException)
            {
                throw new BoardStateException("Could not confirm the command result; uncertainty remains");
            }
            if (result is null)
                throw new BoardStateException("No final result matches the pending operation; uncertainty remains");
            lock (gate)
            {
                uncertainCommand = null;
                uncertainWirelessOperation = null;
                if (result.Wireless is not null) wirelessReceipt = time.GetTimestamp();
                snapshot = snapshot with
                {
                    CommandOutcomeUncertain = false,
                    RebootPending = snapshot.RebootPending ||
                        result.ControlResult is { Status: ControlStatus.Completed,
                            Opcode: Opcode.Reboot or Opcode.SaveSettings },
                    Error = null,
                    Wireless = result.Wireless ?? snapshot.Wireless,
                    WirelessError = result.Wireless is null ? snapshot.WirelessError : null
                };
            }
            var control = result.ControlResult is { } controlResult
                ? new ControlResultResponse(HidProtocol.ReportVersion, controlResult.TransactionId,
                    (byte)controlResult.Opcode, (byte)controlResult.Status,
                    controlResult.Status.ToString().ToLowerInvariant(), (byte)controlResult.ErrorReason,
                    controlResult.ErrorReason.ToString().ToLowerInvariant(),
                    controlResult.Status is ControlStatus.Invalid or ControlStatus.Busy or ControlStatus.Failed
                        ? controlResult.ResultData[0] : (byte)0)
                : null;
            return new(false, control, result.Wireless);
        }
    }

    /// <summary>Test seam: adopt <paramref name="client"/> with <paramref name="status"/> as just reported.</summary>
    internal void Attach(IHidCommands client, StatusResponse status)
    {
        using (Owner())
        lock (gate)
        {
            activeClient = status.Connected ? client : null;
            snapshot = status;
            uncertainCommand = null;
            uncertainWirelessOperation = null;
            telemetryReceipt = storageReceipt = selfTestReceipt = time.GetTimestamp();
            wirelessReceipt = status.Wireless is null ? 0 : time.GetTimestamp();
        }
    }

    // Board-state rules live here; the endpoints validate request shape and protocol ranges once, before the board.
    public void SetLed(bool on, CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client(requireConfiguration: false);
            if (Snapshot.Runtime?.LedAvailable != true)
                throw new BoardStateException("The board LED is unavailable");
            ExecuteCommand(client, () => client.SetLed(on));
            lock (gate)
                if (snapshot.Runtime is { } runtime) snapshot = snapshot with { Runtime = runtime with { LedOn = on } };
        }
    }

    public MscStatusResponse SetMsc(bool enabled, bool force = false, CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client();
            if (force && Snapshot.Memory?.Msc is not { Enabled: true, TransitionPending: false })
                throw new BoardStateException("Force return requires enabled MSC");
            ExecuteCommand(client, () => client.SetMsc(enabled, force));
            lock (gate)
            {
                var changingOwner = (snapshot.Memory?.Msc?.Enabled ?? false) != enabled;
                var msc = new MscStatusResponse(snapshot.Memory?.Msc?.Enabled ?? false, true, false);
                if (snapshot.Memory is { } memory) snapshot = snapshot with { Memory = memory with { Msc = msc } };
                mscReconnectDeadline = changingOwner ? time.GetTimestamp() + time.TimestampFrequency * 5 : 0;
                mscReconnectEnabled = changingOwner ? enabled : null;
                return msc;
            }
        }
    }

    public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false,
        CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client();
            var current = Snapshot;
            if (!current.SettingsSupported)
                throw new BoardStateException("Boot settings are unavailable on this firmware");
            RequireIdleStorage(current, "saving boot settings", force);
            var saved = ExecuteCommand(client, () => client.SetSettings(settings, force));
            lock (gate)
                snapshot = snapshot with
                {
                    BoardConfiguration = snapshot.BoardConfiguration! with { NextBoot = saved },
                    RebootPending = true,
                    Memory = force && snapshot.Memory is { Msc: { } msc } memory
                        ? memory with { Msc = msc with { Enabled = false } } : snapshot.Memory
                };
            return saved;
        }
    }

    public byte[] TcpCliAuthProof(byte[] nonce, byte[] uid, CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client();
            return ExecuteCommand(client, () => client.TcpCliAuthProof(nonce, uid));
        }
    }

    public WirelessResponse SetWireless(WirelessRequest settings, CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = RequireWirelessIdle();
            var saved = ExecuteCommand(client, () => client.SetWireless(settings));
            lock (gate)
            {
                wirelessReceipt = time.GetTimestamp();
                snapshot = snapshot with
                {
                    BoardConfiguration = snapshot.BoardConfiguration is { } configuration
                        ? configuration with { NextBoot = settings.Board } : null,
                    RebootPending = true,
                    Wireless = saved
                };
            }
            return new(saved, true, settings.Board);
        }
    }

    public IReadOnlyList<WirelessNetworkResponse> ScanWireless(CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = RequireWirelessIdle();
            return ExecuteCommand(client, () => client.ScanWireless(() => PollInputs(client)));
        }
    }

    public WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null,
        CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = RequireWirelessIdle();
            using var stop = new CancellationTokenSource();
            lock (gate) wirelessTestCancel = stop;
            var cancelSent = false;
            try
            {
                return ExecuteCommand(client, () => client.TestWireless(ssid, password, () => PollInputs(client),
                    SetWirelessPhase, ssidHex, stop.Token));
            }
            catch (WirelessTestCancelledException)
            {
                cancelSent = true;
                throw;
            }
            finally
            {
                lock (gate) wirelessTestCancel = null;
                SetWirelessPhase(null);
                // A cancel that arrived after the client's last check must still clear the test credentials.
                if (stop.IsCancellationRequested && !cancelSent && !Snapshot.CommandOutcomeUncertain)
                    ExecuteCommand(client, client.CancelWirelessTest);
            }
        }
    }

    public void CancelWirelessTest(CancellationToken cancel = default)
    {
        // A running test owns the board for up to 35 s; it sends the cancel at its next 0.5 s poll instead.
        lock (gate)
            if (wirelessTestCancel is { } running)
            {
                running.Cancel();
                return;
            }
        using (Device(cancel))
        {
            var client = RequireWirelessIdle();
            ExecuteCommand(client, client.CancelWirelessTest);
        }
    }

    public void Reboot(CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client();
            RequireIdleStorage(Snapshot, "rebooting");
            ExecuteCommand(client, client.Reboot);
            lock (gate) snapshot = snapshot with { RebootPending = true };
        }
    }

    public void SetSelfTest(string action, uint sizeMib, CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client();
            ExecuteCommand(client, () => client.SetSelfTest(action, sizeMib));
        }
    }

    /// <summary>Returns the open handle; call holding deviceGate so it cannot be replaced mid-command.</summary>
    private IHidCommands Client(bool requireConfiguration = true)
    {
        lock (gate)
        {
            var client = activeClient ?? throw new IOException("PicoForge HID is not connected");
            if (requireConfiguration && snapshot.BoardConfiguration is null)
                throw new BoardStateException("Board configuration is unavailable; reconnect before sending commands");
            return client;
        }
    }

    /// <summary>Throws unless firmware owns idle storage, or for a forced save, the host owns it.</summary>
    private static void RequireIdleStorage(StatusResponse status, string action, bool force = false)
    {
        if (status.Memory?.Msc is not { } msc)
            throw new BoardStateException("Waiting for storage ownership status");
        if (force && !msc.Enabled) throw new BoardStateException("Forced save requires USB ownership");
        if (msc.TransitionPending || (msc.Enabled && !force))
            throw new BoardStateException($"Disable the USB drive before {action}");
        if (status.SelfTest?.Active == true)
            throw new BoardStateException($"Wait for the storage self-test to finish before {action}");
    }

    /// <summary>Returns the handle when Wi-Fi staging cannot race storage, self-test or an unresolved write.</summary>
    private IHidCommands RequireWirelessIdle()
    {
        var client = Client();
        var current = Snapshot;
        if (current.Hardware?.WirelessSupported != true)
            throw new BoardStateException("Wireless provisioning is unavailable on this firmware");
        if (current.Memory?.Msc is not { Enabled: false, TransitionPending: false } ||
            current.SelfTest?.Active == true ||
            ((current.Hardware.Capabilities & HidProtocol.CapabilitySelfTest) != 0 && current.SelfTest is null))
            throw new BoardStateException(
                "Wireless scan and connection test require idle firmware-owned storage");
        return client;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        // ponytail: Yield before synchronous HID discovery so first API access does not wait for device startup.
        await Task.Yield();
        while (!stoppingToken.IsCancellationRequested && Volatile.Read(ref stopping) == 0)
        {
            IHidClient? client = null;
            try
            {
                client = createClient();
                var hardware = client.ReadInfo();
                BoardConfigurationResponse? configuration;
                string? settingsMessage = null;
                try { configuration = client.ReadConfiguration(); }
                catch (Exception error) when (IsDeviceError(error))
                {
                    logger.LogWarning(error, "Board configuration unavailable; retaining read-only diagnostics");
                    configuration = null;
                    settingsMessage = $"Boot settings unavailable: {error.Message}";
                }
                using (await OwnerAsync(CancellationToken.None))
                lock (gate)
                {
                    activeClient = client;
                    telemetryReceipt = storageReceipt = selfTestReceipt = wirelessReceipt = 0;
                    var uncertain = snapshot.CommandOutcomeUncertain;
                    snapshot = new(true, uncertain ? UncertainMessage : null, hardware,
                        BoardConfiguration: configuration, CommandOutcomeUncertain: uncertain,
                        SettingsMessage: settingsMessage,
                        WirelessError: hardware.WirelessSupported ? settingsMessage : null);
                }

                long wirelessPollAt = 0;
                while (!stoppingToken.IsCancellationRequested)
                {
                    using (await OwnerAsync(stoppingToken))
                    {
                        PollInputs(client);
                        if (hardware.WirelessSupported && configuration is not null &&
                            time.GetTimestamp() >= wirelessPollAt)
                            wirelessPollAt = PollWireless(client);
                    }
                    // ponytail: 100 ms stays below the 250 ms telemetry cadence and halves idle wakeups.
                    await Task.Delay(TimeSpan.FromMilliseconds(100), stoppingToken);
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested) { break; }
            catch (Exception error)
            {
                // Any fault, including a code bug, must leave a visible error and a retrying monitor.
                if (!IsDeviceError(error)) logger.LogError(error, "PicoForge HID monitor failed; retrying");
                HandleDisconnect(error);
            }
            finally
            {
                using (Owner())
                {
                    lock (gate)
                    {
                        if (ReferenceEquals(activeClient, client)) activeClient = null;
                    }
                    try { client?.Dispose(); }
                    catch (Exception error)
                    {
                        logger.LogError(error, "HID cleanup failed; stopping monitor to prevent overlapping handles");
                        cleanupError = error;
                        Interlocked.Exchange(ref stopping, 1);
                        lock (gate) snapshot = new(false, "HID cleanup failed; restart the dashboard",
                            CommandOutcomeUncertain: snapshot.CommandOutcomeUncertain);
                    }
                }
            }

            if (Volatile.Read(ref stopping) != 0) break;
            try { await Task.Delay(TimeSpan.FromSeconds(1), stoppingToken); }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested) { break; }
        }
    }

    private void SetWirelessPhase(string? phase)
    {
        lock (gate) snapshot = snapshot with { WirelessPhase = phase };
    }

    /// <summary>
    /// Publishes Feature 12 into the snapshot; returns the next poll time (0.5 s while active, else 2 s).
    /// </summary>
    private long PollWireless(IHidCommands client)
    {
        var active = false;
        try
        {
            var wireless = client.ReadWireless();
            active = wireless.WifiState == WifiState.Testing ||
                (WirelessScanState)wireless.ScanState == WirelessScanState.Scanning;
            lock (gate)
            {
                wirelessReceipt = time.GetTimestamp();
                snapshot = snapshot with { Wireless = wireless, WirelessError = null };
            }
        }
        catch (Exception error) when (IsDeviceError(error))
        {
            lock (gate) snapshot = snapshot with { WirelessError = error.Message };
        }
        return time.GetTimestamp() + time.TimestampFrequency * (active ? 1 : 4) / 2;
    }

    private void PollInputs(IHidCommands client)
    {
        for (var i = 0; i < 16; i++)
        {
            var report = client.ReadInputReport(0);
            if (report is null) break;
            ApplyInputReport(report);
        }
    }

    /// <summary>Merges one report; freshness is derived on read by <see cref="Fresh"/>.</summary>
    private void ApplyInputReport(InputReport report)
    {
        var receivedAt = time.GetTimestamp();
        lock (gate)
        {
            if (report.Runtime is not null) telemetryReceipt = receivedAt;
            if (report.Memory is not null) storageReceipt = receivedAt;
            if (report.SelfTest is not null) selfTestReceipt = receivedAt;
            if (report.Memory?.Msc is { TransitionPending: false } ownership
                && ownership.Enabled == mscReconnectEnabled)
            {
                mscReconnectDeadline = 0;
                mscReconnectEnabled = null;
            }
            snapshot = snapshot with
            {
                Runtime = report.Runtime ?? snapshot.Runtime,
                Memory = report.Memory ?? snapshot.Memory,
                SelfTest = report.SelfTest ?? snapshot.SelfTest,
                UpdatedAt = time.GetUtcNow()
            };
        }
    }

    /// <summary>Call under gate: the snapshot with stale sections hidden, judged at read time.</summary>
    private StatusResponse Fresh()
    {
        if (!snapshot.Connected) return snapshot;
        var telemetryStale = IsStale(telemetryReceipt, 750);
        var storageStale = IsStale(storageReceipt, 1500);
        var selfTestStale = IsStale(selfTestReceipt, 1500);
        return snapshot with
        {
            Runtime = telemetryStale ? null : snapshot.Runtime,
            Memory = storageStale ? null : snapshot.Memory,
            SelfTest = selfTestStale ? null : snapshot.SelfTest,
            TelemetryStale = telemetryStale,
            StorageStale = storageStale,
            SelfTestStale = selfTestStale,
            WirelessError = snapshot.WirelessError ??
                (snapshot.Wireless is not null && IsStale(wirelessReceipt, 5000)
                    ? "Wireless status is stale; wait for a fresh report" : null)
        };
    }

    internal void HandleDisconnect(Exception error)
    {
        using (Owner())
        lock (gate)
        {
            // Only a lost or timed-out handle during a confirmed USB-drive switch is an expected reconnect.
            var uncertain = snapshot.CommandOutcomeUncertain;
            var reconnecting = !uncertain && time.GetTimestamp() < mscReconnectDeadline &&
                error is IOException or TimeoutException;
            if (reconnecting)
                logger.LogInformation(error, "USB-drive mode change: waiting for HID reconnection");
            activeClient = null;
            telemetryReceipt = storageReceipt = selfTestReceipt = 0;
            snapshot = new(false, uncertain ? UncertainMessage
                    : reconnecting ? null
                    : IsDeviceError(error) ? error.Message : $"Internal monitor error: {error.Message}",
                CommandOutcomeUncertain: uncertain, Reconnecting: reconnecting);
        }
    }

    // Board refusals are typed separately; unclassified exceptions are logged as monitor faults.
    private static bool IsDeviceError(Exception error) => error is IOException or InvalidDataException
        or UnauthorizedAccessException or TimeoutException;

    private bool IsStale(long receivedAt, int maximumAgeMs) =>
        receivedAt == 0 || time.GetElapsedTime(receivedAt).TotalMilliseconds >= maximumAgeMs;

    private void ExecuteCommand(IHidCommands client, Action command) =>
        ExecuteCommand(client, () => { command(); return true; });

    // ponytail: a command blocks one thread-pool thread while it holds deviceGate: up to 35 s for a Wi-Fi test, 15 s
    // for a scan and 5 s for a forced save. Other commands give up after BusyWait (409) and a cancel reaches a running
    // test without waiting, so one user per board never queues. If many clients share a board, run Wi-Fi scan and
    // test in the background and publish their results through /api/status instead.
    private T ExecuteCommand<T>(IHidCommands client, Func<T> command)
    {
        if (Snapshot.CommandOutcomeUncertain) throw new BoardStateException(UncertainMessage);
        if (Snapshot.RebootPending) throw new BoardStateException("Board reboot is pending; wait for reconnection");
        try
        {
            return command();
        }
        catch
        {
            if (client.CommandOutcomeUncertain)
            {
                uncertainCommand = client.UncertainCommand;
                uncertainWirelessOperation = client.UncertainWirelessOperation;
                lock (gate) snapshot = snapshot with { Error = UncertainMessage, CommandOutcomeUncertain = true };
            }
            throw;
        }
    }
}
