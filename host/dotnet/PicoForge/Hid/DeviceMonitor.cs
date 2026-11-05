using System.Diagnostics;
using Microsoft.Extensions.Logging.Abstractions;

namespace PicoForge;

/// <summary>One board's monitor. Commands throw instead of returning a flag: see <c>UseApiErrors</c>.</summary>
internal interface IDeviceMonitor : IHostedService, IDisposable
{
    StatusResponse Snapshot { get; }
    void SetLed(bool on);
    MscStatusResponse SetMsc(bool enabled, bool force = false);
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false);
    void Reboot();
    void SetSelfTest(string action, uint sizeMib);
    WirelessResponse ReadWireless();
    WirelessResponse SetWireless(WirelessRequest settings);
    IReadOnlyList<WirelessNetworkResponse> ScanWireless();
    WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null);
    void CancelWirelessTest();
    byte[] TcpCliAuthProof(byte[] nonce, byte[] uid) =>
        throw new NotSupportedException("USB TCP CLI authentication is unavailable");
}

/// <summary>Serialized HID operations used by <see cref="DeviceMonitor"/>; allows tests to block a write.</summary>
internal interface IHidCommands
{
    bool CommandOutcomeUncertain { get; }
    InputReport? ReadInputReport(int timeoutMilliseconds = 250);
    void SetLed(bool on);
    void SetMsc(bool enabled, bool force = false);
    BoardConfigurationResponse ReadConfiguration();
    BootSettingsResponse ReadSettings();
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false);
    void Reboot();
    void SetSelfTest(string action, uint sizeMib);
    WirelessStatusResponse ReadWireless();
    byte[] TcpCliAuthProof(byte[] nonce, byte[] uid) =>
        throw new NotSupportedException("USB TCP CLI authentication is unavailable");
    WirelessStatusResponse SetWireless(WirelessRequest settings);
    IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null);
    WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
        Action<string>? onProgress = null, string? ssidHex = null);
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
    private readonly Func<IHidClient> createClient;
    private readonly ILogger<DeviceMonitor> logger;
    private readonly object gate = new();
    // Handle operations always acquire deviceGate before the snapshot gate.
    private readonly object deviceGate = new();
    private IHidCommands? activeClient;
    // Stopwatch timestamps of the last report per section, guarded by gate; 0 means none since connecting.
    private long telemetryReceipt;
    private long storageReceipt;
    private long selfTestReceipt;
    private bool commandOutcomeUncertain;
    private long mscReconnectDeadline;
    private bool? mscReconnectEnabled;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null, null, null, null);

    public DeviceMonitor() : this((string?)null, null) { }

    public DeviceMonitor(string? serialNumber, string? devicePath, ILogger<DeviceMonitor>? logger = null)
        : this(() => new PicoForgeHid(serialNumber, devicePath), logger ?? NullLogger<DeviceMonitor>.Instance) { }

    internal DeviceMonitor(Func<IHidClient> createClient, ILogger<DeviceMonitor>? logger = null)
    {
        this.createClient = createClient;
        this.logger = logger ?? NullLogger<DeviceMonitor>.Instance;
    }

    /// <summary>The latest reports, with any section older than its freshness limit hidden.</summary>
    public StatusResponse Snapshot
    {
        get { lock (gate) return Fresh(); }
    }

    /// <summary>Test seam: adopt <paramref name="client"/> with <paramref name="status"/> as just reported.</summary>
    internal void Attach(IHidCommands client, StatusResponse status)
    {
        lock (deviceGate)
        lock (gate)
        {
            activeClient = status.Connected ? client : null;
            snapshot = status;
            commandOutcomeUncertain = status.CommandOutcomeUncertain;
            telemetryReceipt = storageReceipt = selfTestReceipt = Stopwatch.GetTimestamp();
        }
    }

    public void SetLed(bool on)
    {
        lock (deviceGate)
        {
            if (Snapshot.BoardStatus?.LedAvailable != true)
                throw new InvalidOperationException("PicoForge HID is disconnected or the board LED is unavailable");
            var client = Client();
            ExecuteCommand(client, () => client.SetLed(on));
            lock (gate)
                if (snapshot.Runtime is { } runtime)
                {
                    runtime = runtime with { LedOn = on };
                    snapshot = snapshot with { Runtime = runtime, BoardStatus = runtime.ToBoardStatus() };
                }
        }
    }

    public MscStatusResponse SetMsc(bool enabled, bool force = false)
    {
        if (force && enabled) throw new InvalidOperationException("Force return requires disabling MSC");
        lock (deviceGate)
        {
            var client = Client();
            if (force && Snapshot.Msc is not { Enabled: true, TransitionPending: false })
                throw new InvalidOperationException("Force return requires enabled MSC");
            ExecuteCommand(client, () => client.SetMsc(enabled, force));
            lock (gate)
            {
                var changingOwner = (snapshot.Msc?.Enabled ?? false) != enabled;
                var msc = new MscStatusResponse(snapshot.Msc?.Enabled ?? false, true, false);
                snapshot = snapshot with { Msc = msc };
                mscReconnectDeadline = changingOwner ? Environment.TickCount64 + 5000 : 0;
                mscReconnectEnabled = changingOwner ? enabled : null;
                return msc;
            }
        }
    }

    public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
    {
        if (settings.SystemClockMhz is not (0 or 150 or 200 or 250) || settings.SdCardProfile is not (0 or 1) ||
            settings.Radios is not (>= 0 and <= 3))
            throw new ArgumentException("Unsupported boot settings value");
        lock (deviceGate)
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
                    Settings = saved,
                    RebootPending = true,
                    Msc = force ? snapshot.Msc! with { Enabled = false } : snapshot.Msc
                };
            return saved;
        }
    }

    public WirelessResponse ReadWireless()
    {
        lock (deviceGate)
            return new(Client().ReadWireless(), Snapshot.RebootPending);
    }

    public byte[] TcpCliAuthProof(byte[] nonce, byte[] uid)
    {
        lock (deviceGate)
        {
            var client = Client();
            return ExecuteCommand(client, () => client.TcpCliAuthProof(nonce, uid));
        }
    }

    public WirelessResponse SetWireless(WirelessRequest settings)
    {
        HidProtocol.ValidateWirelessRequest(settings);
        lock (deviceGate)
        {
            var client = RequireWirelessIdle();
            var saved = ExecuteCommand(client, () => client.SetWireless(settings));
            lock (gate) snapshot = snapshot with { Settings = settings.Board, RebootPending = true, Wireless = saved };
            return new(saved, true, settings.Board);
        }
    }

    public IReadOnlyList<WirelessNetworkResponse> ScanWireless()
    {
        lock (deviceGate)
        {
            var client = RequireWirelessIdle();
            return ExecuteCommand(client, () => client.ScanWireless(() => PollInputs(client)));
        }
    }

    public WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null)
    {
        lock (deviceGate)
        {
            var client = RequireWirelessIdle();
            try
            {
                return ExecuteCommand(client, () => client.TestWireless(ssid, password, () => PollInputs(client),
                    SetWirelessPhase, ssidHex));
            }
            finally { SetWirelessPhase(null); }
        }
    }

    public void CancelWirelessTest()
    {
        lock (deviceGate)
        {
            var client = RequireWirelessIdle();
            ExecuteCommand(client, client.CancelWirelessTest);
        }
    }

    public void Reboot()
    {
        lock (deviceGate)
        {
            var client = Client();
            RequireIdleStorage(Snapshot, "rebooting");
            ExecuteCommand(client, client.Reboot);
        }
    }

    public void SetSelfTest(string action, uint sizeMib)
    {
        lock (deviceGate)
        {
            var client = Client();
            if (((Snapshot.Hardware?.Capabilities ?? 0) & HidProtocol.CapabilitySelfTest) == 0)
                throw new InvalidOperationException("PicoForge HID self-test interface is unavailable");
            ExecuteCommand(client, () => client.SetSelfTest(action, sizeMib));
        }
    }

    /// <summary>Returns the open handle; call under deviceGate so it cannot be replaced mid-command.</summary>
    private IHidCommands Client()
    {
        lock (gate) return activeClient ?? throw new IOException("PicoForge HID is not connected");
    }

    /// <summary>Throws unless firmware owns idle storage, or for a forced save, the host owns it.</summary>
    private static void RequireIdleStorage(StatusResponse status, string action, bool force = false)
    {
        if (status.Msc is not { } msc) throw new InvalidOperationException("Waiting for storage ownership status");
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
        if (current.Msc is not { Enabled: false, TransitionPending: false } ||
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
                var settings = HidProtocol.ToBootSettings(configuration);
                lock (deviceGate)
                lock (gate)
                {
                    activeClient = client;
                    telemetryReceipt = storageReceipt = selfTestReceipt = 0;
                    snapshot = new(true, commandOutcomeUncertain ? UncertainMessage : null,
                        hardware, null, null, null, null, null,
                        Settings: settings, BoardConfiguration: configuration,
                        CommandOutcomeUncertain: commandOutcomeUncertain);
                }

                long wirelessPollAt = 0;
                while (!stoppingToken.IsCancellationRequested)
                {
                    lock (deviceGate)
                    {
                        PollInputs(client);
                        if (hardware.WirelessSupported && Stopwatch.GetTimestamp() >= wirelessPollAt)
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
                lock (deviceGate)
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

    /// <summary>Publishes Feature 12 into the snapshot; returns the next poll time (0.5 s while active, else 2 s).</summary>
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
        return Stopwatch.GetTimestamp() + Stopwatch.Frequency * (active ? 1 : 4) / 2;
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
        var receivedAt = Stopwatch.GetTimestamp();
        lock (gate)
        {
            if (report.Runtime is not null) telemetryReceipt = receivedAt;
            if (report.Memory is not null) storageReceipt = receivedAt;
            if (report.SelfTest is not null) selfTestReceipt = receivedAt;
            if (report.Msc is { TransitionPending: false } ownership && ownership.Enabled == mscReconnectEnabled)
            {
                mscReconnectDeadline = 0;
                mscReconnectEnabled = null;
            }
            snapshot = snapshot with
            {
                Runtime = report.Runtime ?? snapshot.Runtime,
                BoardStatus = report.Runtime?.ToBoardStatus() ?? snapshot.BoardStatus,
                Memory = report.Memory ?? snapshot.Memory,
                Msc = report.Msc ?? snapshot.Msc,
                FilesystemFormat = report.Memory is null ? snapshot.FilesystemFormat : report.FilesystemFormat,
                SelfTest = report.SelfTest ?? snapshot.SelfTest,
                UpdatedAt = DateTimeOffset.UtcNow
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
            BoardStatus = telemetryStale ? null : snapshot.BoardStatus,
            Memory = storageStale ? null : snapshot.Memory,
            Msc = storageStale ? null : snapshot.Msc,
            FilesystemFormat = storageStale ? null : snapshot.FilesystemFormat,
            SelfTest = selfTestStale ? null : snapshot.SelfTest,
            TelemetryStale = telemetryStale,
            StorageStale = storageStale,
            SelfTestStale = selfTestStale
        };
    }

    internal void HandleDisconnect(Exception error)
    {
        lock (deviceGate)
        lock (gate)
        {
            // Only a lost or timed-out handle during a confirmed USB-drive switch is an expected reconnect.
            var reconnecting = !commandOutcomeUncertain && Environment.TickCount64 < mscReconnectDeadline &&
                error is IOException or TimeoutException;
            if (reconnecting)
                logger.LogInformation(error, "USB-drive mode change: waiting for HID reconnection");
            activeClient = null;
            telemetryReceipt = storageReceipt = selfTestReceipt = 0;
            snapshot = new(false, commandOutcomeUncertain ? UncertainMessage
                    : reconnecting ? null
                    : IsDeviceError(error) ? error.Message : $"Internal monitor error: {error.Message}",
                null, null, null, null, null, null,
                CommandOutcomeUncertain: commandOutcomeUncertain, Reconnecting: reconnecting);
        }
    }

    // InvalidDataException is malformed board data; InvalidOperationException is a board-state or firmware refusal.
    private static bool IsDeviceError(Exception error) => error is IOException or InvalidDataException
        or InvalidOperationException or UnauthorizedAccessException or NotSupportedException or TimeoutException;

    private static bool IsStale(long receivedAt, int maximumAgeMs) =>
        receivedAt == 0 || Stopwatch.GetElapsedTime(receivedAt).TotalMilliseconds >= maximumAgeMs;

    private void ExecuteCommand(IHidCommands client, Action command) =>
        ExecuteCommand(client, () => { command(); return true; });

    // ponytail: commands block one thread-pool thread for up to 5 s (forced save) under deviceGate. That is fine for a
    // localhost dashboard with one user per board; move HID I/O to a dedicated thread with a queue if it ever is not.
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
