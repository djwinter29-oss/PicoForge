using Microsoft.Extensions.Logging.Abstractions;

namespace PicoForge;

/// <summary>
/// One board's monitor. Commands throw instead of returning a flag: see <c>UseApiErrors</c>. <c>cancel</c> stops only
/// the wait for the board (an aborted request); a command that reached the board always runs to completion.
/// </summary>
internal interface IDeviceMonitor : IDisposable
{
    StatusResponse Snapshot { get; }
    void Start();
    void SetLed(bool on, CancellationToken cancel = default);
    MscStatusResponse SetMsc(bool enabled, bool force = false, CancellationToken cancel = default);
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false,
        CancellationToken cancel = default);
    void Reboot(CancellationToken cancel = default);
    void SetSelfTest(string action, uint sizeMib, CancellationToken cancel = default);
    WirelessResponse ReadWireless(CancellationToken cancel = default);
    WirelessResponse SetWireless(WirelessRequest settings, CancellationToken cancel = default);
    IReadOnlyList<WirelessNetworkResponse> ScanWireless(CancellationToken cancel = default);
    WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null,
        CancellationToken cancel = default);
    void CancelWirelessTest(CancellationToken cancel = default);
    byte[] TcpCliAuthProof(byte[] nonce, byte[] uid, CancellationToken cancel = default);
}

/// <summary>The board is running another command; nothing was sent, so retrying is safe.</summary>
internal sealed class BoardBusyException()
    : InvalidOperationException("Board is busy with another command; retry when it finishes");

/// <summary>Serialized HID operations used by <see cref="DeviceMonitor"/>; allows tests to block a write.</summary>
internal interface IHidCommands
{
    bool CommandOutcomeUncertain { get; }
    InputReport? ReadInputReport(int timeoutMilliseconds = 250);
    void SetLed(bool on);
    void SetMsc(bool enabled, bool force = false);
    BoardConfigurationResponse ReadConfiguration();
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false);
    void Reboot();
    void SetSelfTest(string action, uint sizeMib);
    WirelessStatusResponse ReadWireless();
    byte[] TcpCliAuthProof(byte[] nonce, byte[] uid) =>
        throw new NotSupportedException("USB TCP CLI authentication is unavailable");
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
    // Handle operations always acquire deviceGate before the snapshot gate. Not reentrant.
    private readonly SemaphoreSlim deviceGate = new(1, 1);
    private IHidCommands? activeClient;
    // TimeProvider timestamps of the last report per section, guarded by gate; 0 means none since connecting.
    private long telemetryReceipt;
    private long storageReceipt;
    private long selfTestReceipt;
    private bool commandOutcomeUncertain;
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

    public void Start() => _ = StartAsync(CancellationToken.None);

    /// <summary>Holds <see cref="deviceGate"/> until disposed.</summary>
    private readonly struct DeviceLease(SemaphoreSlim gate) : IDisposable
    {
        public void Dispose() => gate.Release();
    }

    /// <summary>
    /// API commands wait at most <see cref="BusyWait"/>; a busy board is a 409 because nothing was sent.
    /// </summary>
    private DeviceLease Device(CancellationToken cancel)
    {
        if (!deviceGate.Wait(BusyWait, cancel)) throw new BoardBusyException();
        return new(deviceGate);
    }

    /// <summary>The monitor loop owns the device, so it waits without a limit.</summary>
    private DeviceLease Owner()
    {
        deviceGate.Wait();
        return new(deviceGate);
    }

    /// <summary>The latest reports, with any section older than its freshness limit hidden.</summary>
    public StatusResponse Snapshot
    {
        get { lock (gate) return Fresh(); }
    }

    /// <summary>Test seam: adopt <paramref name="client"/> with <paramref name="status"/> as just reported.</summary>
    internal void Attach(IHidCommands client, StatusResponse status)
    {
        using (Owner())
        lock (gate)
        {
            activeClient = status.Connected ? client : null;
            snapshot = status;
            commandOutcomeUncertain = status.CommandOutcomeUncertain;
            telemetryReceipt = storageReceipt = selfTestReceipt = time.GetTimestamp();
        }
    }

    // Board-state rules live here; the endpoints validate request shape and protocol ranges once, before the board.
    public void SetLed(bool on, CancellationToken cancel = default)
    {
        using (Device(cancel))
        {
            var client = Client();
            if (Snapshot.Runtime?.LedAvailable != true)
                throw new InvalidOperationException("The board LED is unavailable");
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
                throw new InvalidOperationException("Force return requires enabled MSC");
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
                throw new InvalidOperationException("Boot settings are unavailable on this firmware");
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

    public WirelessResponse ReadWireless(CancellationToken cancel = default)
    {
        using (Device(cancel))
            return new(Client().ReadWireless(), Snapshot.RebootPending);
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
                snapshot = snapshot with
                {
                    BoardConfiguration = snapshot.BoardConfiguration is { } configuration
                        ? configuration with { NextBoot = settings.Board } : null,
                    RebootPending = true,
                    Wireless = saved
                };
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
                if (stop.IsCancellationRequested && !cancelSent && !commandOutcomeUncertain)
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
    private IHidCommands Client()
    {
        lock (gate) return activeClient ?? throw new IOException("PicoForge HID is not connected");
    }

    /// <summary>Throws unless firmware owns idle storage, or for a forced save, the host owns it.</summary>
    private static void RequireIdleStorage(StatusResponse status, string action, bool force = false)
    {
        if (status.Memory?.Msc is not { } msc)
            throw new InvalidOperationException("Waiting for storage ownership status");
        if (force && !msc.Enabled) throw new InvalidOperationException("Forced save requires USB ownership");
        if (msc.TransitionPending || (msc.Enabled && !force))
            throw new InvalidOperationException($"Disable the USB drive before {action}");
        if (status.SelfTest?.Active == true)
            throw new InvalidOperationException($"Wait for the storage self-test to finish before {action}");
    }

    /// <summary>Returns the handle when Wi-Fi staging cannot race storage, self-test or an unresolved write.</summary>
    private IHidCommands RequireWirelessIdle()
    {
        var client = Client();
        var current = Snapshot;
        if (current.Hardware?.WirelessSupported != true)
            throw new InvalidOperationException("Wireless provisioning is unavailable on this firmware");
        if (current.CommandOutcomeUncertain) throw new InvalidOperationException(UncertainMessage);
        if (current.Memory?.Msc is not { Enabled: false, TransitionPending: false } ||
            current.RebootPending || current.SelfTest?.Active == true ||
            ((current.Hardware.Capabilities & HidProtocol.CapabilitySelfTest) != 0 && current.SelfTest is null))
            throw new InvalidOperationException(
                "Wireless scan and connection test require idle firmware-owned storage");
        return client;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        // ponytail: Yield before synchronous HID discovery so first API access does not wait for device startup.
        await Task.Yield();
        while (!stoppingToken.IsCancellationRequested)
        {
            IHidClient? client = null;
            try
            {
                client = createClient();
                var hardware = client.ReadInfo();
                var configuration = client.ReadConfiguration();
                using (Owner())
                lock (gate)
                {
                    activeClient = client;
                    telemetryReceipt = storageReceipt = selfTestReceipt = 0;
                    snapshot = new(true, commandOutcomeUncertain ? UncertainMessage : null, hardware,
                        BoardConfiguration: configuration, CommandOutcomeUncertain: commandOutcomeUncertain);
                }

                long wirelessPollAt = 0;
                while (!stoppingToken.IsCancellationRequested)
                {
                    using (Owner())
                    {
                        PollInputs(client);
                        if (hardware.WirelessSupported && time.GetTimestamp() >= wirelessPollAt)
                            wirelessPollAt = PollWireless(client);
                    }
                    // ponytail: 50 ms stays below the 250 ms telemetry cadence and cuts idle wakeups by 80%.
                    await Task.Delay(TimeSpan.FromMilliseconds(50), stoppingToken);
                }
            }
            catch (Exception error) when (!stoppingToken.IsCancellationRequested)
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
                    client?.Dispose();
                }
            }

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
            active = wireless.WifiState == "testing" || wireless.ScanState == 1;
            lock (gate) snapshot = snapshot with { Wireless = wireless, WirelessError = null };
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
            SelfTestStale = selfTestStale
        };
    }

    internal void HandleDisconnect(Exception error)
    {
        using (Owner())
        lock (gate)
        {
            // Only a lost or timed-out handle during a confirmed USB-drive switch is an expected reconnect.
            var reconnecting = !commandOutcomeUncertain && time.GetTimestamp() < mscReconnectDeadline &&
                error is IOException or TimeoutException;
            if (reconnecting)
                logger.LogInformation(error, "USB-drive mode change: waiting for HID reconnection");
            activeClient = null;
            telemetryReceipt = storageReceipt = selfTestReceipt = 0;
            snapshot = new(false, commandOutcomeUncertain ? UncertainMessage
                    : reconnecting ? null
                    : IsDeviceError(error) ? error.Message : $"Internal monitor error: {error.Message}",
                CommandOutcomeUncertain: commandOutcomeUncertain, Reconnecting: reconnecting);
        }
    }

    // InvalidDataException is malformed board data; InvalidOperationException is a board-state or firmware refusal.
    private static bool IsDeviceError(Exception error) => error is IOException or InvalidDataException
        or InvalidOperationException or UnauthorizedAccessException or NotSupportedException or TimeoutException;

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
        if (commandOutcomeUncertain) throw new InvalidOperationException(UncertainMessage);
        try
        {
            return command();
        }
        catch
        {
            if (client.CommandOutcomeUncertain)
            {
                commandOutcomeUncertain = true;
                lock (gate) snapshot = snapshot with { Error = UncertainMessage, CommandOutcomeUncertain = true };
            }
            throw;
        }
    }
}
