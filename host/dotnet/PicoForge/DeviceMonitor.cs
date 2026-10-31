using System.Diagnostics;
using Microsoft.Extensions.Logging.Abstractions;

namespace PicoForge;

internal interface IDeviceMonitor
{
    StatusResponse Snapshot { get; }
    bool SetLed(bool on);
    MscStatusResponse? SetMsc(bool enabled, bool force = false);
    BootSettingsResponse? SetSettings(BootSettingsResponse settings, bool force = false);
    bool Reboot();
    bool SetSelfTest(string action, uint sizeMib);
    WirelessResponse ReadWireless() => throw new InvalidOperationException("Wireless provisioning unavailable");
    WirelessResponse SetWireless(WirelessRequest settings) => throw new InvalidOperationException("Wireless provisioning unavailable");
}

/// <summary>HID command writes used by <see cref="DeviceMonitor"/>; allows tests to block a write.</summary>
internal interface IHidCommands
{
    bool CommandOutcomeUncertain { get; }
    void SetLed(bool on);
    void SetMsc(bool enabled, bool force = false);
    BoardConfigurationResponse ReadConfiguration();
    BootSettingsResponse ReadSettings();
    BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false);
    void Reboot();
    void SetSelfTest(string action, uint sizeMib);
    WirelessStatusResponse ReadWireless() => throw new InvalidOperationException("Wireless provisioning unavailable");
    WirelessStatusResponse SetWireless(WirelessRequest settings) => throw new InvalidOperationException("Wireless provisioning unavailable");
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private readonly Func<PicoForgeHid> createClient;
    private readonly ILogger<DeviceMonitor> logger;
    private readonly object gate = new();
    // Handle operations always acquire deviceGate before the snapshot gate.
    private readonly object deviceGate = new();
    private IHidCommands? activeClient;
    private long telemetryReceipt;
    private long storageReceipt;
    private long selfTestReceipt;
    private bool commandOutcomeUncertain;
    private long mscReconnectDeadline;
    private bool? mscReconnectEnabled;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null, null, null, null);

    public DeviceMonitor() : this(null, null) { }

    public DeviceMonitor(string? serialNumber, string? devicePath, ILogger<DeviceMonitor>? logger = null)
    {
        createClient = () => new PicoForgeHid(serialNumber, devicePath);
        this.logger = logger ?? NullLogger<DeviceMonitor>.Instance;
    }

    public StatusResponse Snapshot
    {
        get { lock (gate) return snapshot; }
    }

    // deviceGate keeps activeClient stable, so the write runs outside the snapshot gate without going stale.
    public bool SetLed(bool on)
    {
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null || snapshot.BoardStatus?.LedAvailable != true) return false;
                client = activeClient;
            }
            ExecuteCommand(client, () => client.SetLed(on));
            lock (gate)
                snapshot = snapshot with { BoardStatus = snapshot.BoardStatus! with { LedOn = on } };
            return true;
        }
    }

    public MscStatusResponse? SetMsc(bool enabled, bool force = false)
    {
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null) return null;
                if (force && (enabled || snapshot.Msc is not
                    { Enabled: true, TransitionPending: false, ForceReturnSupported: true })) return null;
                client = activeClient;
            }
            ExecuteCommand(client, () => client.SetMsc(enabled, force));
            lock (gate)
            {
                var changingOwner = (snapshot.Msc?.Enabled ?? false) != enabled;
                snapshot = snapshot with
                {
                    Msc = new MscStatusResponse(snapshot.Msc?.Enabled ?? false, true, false,
                        snapshot.Msc?.ForceReturnSupported ?? false)
                };
                mscReconnectDeadline = changingOwner ? Environment.TickCount64 + 5000 : 0;
                mscReconnectEnabled = changingOwner ? enabled : null;
                return snapshot.Msc;
            }
        }
    }

    public BootSettingsResponse? SetSettings(BootSettingsResponse settings, bool force = false)
    {
        if (settings.SystemClockMhz is not (0 or 150 or 200 or 250) || settings.SdCardProfile is < 0 or > 1 ||
            settings.Radios is < 0 or > 3)
            throw new ArgumentOutOfRangeException(nameof(settings));
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null || snapshot.Settings is null || snapshot.Msc is null ||
                    snapshot.Msc.TransitionPending || snapshot.SelfTest?.Active == true ||
                    (force ? !snapshot.Connected || !snapshot.Msc.Enabled ||
                        (snapshot.Hardware?.Capabilities & HidProtocol.CapabilityForcedSave) !=
                            HidProtocol.CapabilityForcedSave : snapshot.Msc.Enabled))
                    return null;
                client = activeClient;
            }
            var saved = ExecuteCommand(client, () => client.SetSettings(settings, force));
            lock (gate)
            {
                snapshot = snapshot with
                {
                    Settings = saved,
                    RebootPending = true,
                    Msc = force ? snapshot.Msc! with { Enabled = false } : snapshot.Msc
                };
                return saved;
            }
        }
    }

    public WirelessResponse ReadWireless()
    {
        lock (deviceGate)
        {
            if (activeClient is null) throw new InvalidOperationException("PicoForge HID is not connected");
            return new(activeClient.ReadWireless(), Snapshot.RebootPending);
        }
    }

    public WirelessResponse SetWireless(WirelessRequest settings)
    {
        HidProtocol.BuildWirelessRequest(settings, 1);
        lock (deviceGate)
        {
            var current = Snapshot;
            if (activeClient is null || !current.Connected || current.Hardware?.WirelessSupported != true)
                throw new InvalidOperationException("Wireless provisioning is unavailable on this firmware");
            if (current.CommandOutcomeUncertain || current.Msc is not { Enabled: false, TransitionPending: false } ||
                current.RebootPending || current.SelfTest?.Active == true ||
                ((current.Hardware.Capabilities & HidProtocol.CapabilitySelfTest) != 0 && current.SelfTest is null))
                throw new InvalidOperationException("Wireless provisioning is unavailable while storage or the board is busy");
            var saved = ExecuteCommand(activeClient, () => activeClient.SetWireless(settings));
            lock (gate) snapshot = snapshot with { RebootPending = true };
            return new(saved, true);
        }
    }

    public bool Reboot()
    {
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null || snapshot.Msc is null || snapshot.Msc.Enabled
                    || snapshot.Msc.TransitionPending || snapshot.SelfTest?.Active == true)
                    return false;
                client = activeClient;
            }
            ExecuteCommand(client, client.Reboot);
            return true;
        }
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            PicoForgeHid? client = null;
            try
            {
                client = createClient();
                var hardware = client.ReadInfo();
                var configuration = client.ReadConfiguration();
                var settings = HidProtocol.ToBootSettings(configuration);
                telemetryReceipt = storageReceipt = selfTestReceipt = 0;
                lock (deviceGate)
                lock (gate)
                {
                    activeClient = client;
                    snapshot = new(true, commandOutcomeUncertain
                            ? "A prior HID command outcome is uncertain; further writes are blocked" : null,
                        hardware, null, null, null, null, null,
                        Settings: settings, BoardConfiguration: configuration,
                        CommandOutcomeUncertain: commandOutcomeUncertain);
                }

                while (!stoppingToken.IsCancellationRequested)
                {
                    lock (deviceGate)
                    {
                        var report = client.ReadInputReport();
                        var now = DateTimeOffset.UtcNow;
                        if (report?.Runtime is not null) telemetryReceipt = Stopwatch.GetTimestamp();
                        if (report?.Memory is not null) storageReceipt = Stopwatch.GetTimestamp();
                        if (report?.SelfTest is not null) selfTestReceipt = Stopwatch.GetTimestamp();
                        lock (gate)
                        {
                            if (report?.Msc is { TransitionPending: false } ownership &&
                                ownership.Enabled == mscReconnectEnabled)
                            {
                                mscReconnectDeadline = 0;
                                mscReconnectEnabled = null;
                            }
                            var telemetryStale = IsStale(telemetryReceipt, 750);
                            var storageStale = IsStale(storageReceipt, 1500);
                            var selfTestStale = IsStale(selfTestReceipt, 1500);
                            var runtime = telemetryStale ? null : report?.Runtime ?? snapshot.Runtime;
                            var memory = storageStale ? null : report?.Memory ?? snapshot.Memory;
                            var msc = storageStale ? null : report?.Msc ?? snapshot.Msc;
                            snapshot = snapshot with
                            {
                                Connected = true,
                                Error = commandOutcomeUncertain
                                    ? "A prior HID command outcome is uncertain; further writes are blocked" : null,
                                Runtime = runtime,
                                BoardStatus = runtime?.ToBoardStatus(),
                                Memory = memory,
                                Msc = msc,
                                SelfTest = selfTestStale ? null : report?.SelfTest ?? snapshot.SelfTest,
                                FilesystemFormat = storageStale ? null
                                    : report?.FilesystemFormat ?? snapshot.FilesystemFormat,
                                UpdatedAt = report is null ? snapshot.UpdatedAt : now,
                                TelemetryUpdatedAt = report?.Runtime is null
                                    ? snapshot.TelemetryUpdatedAt : now,
                                StorageUpdatedAt = report?.Memory is null
                                    ? snapshot.StorageUpdatedAt : now,
                                SelfTestUpdatedAt = report?.SelfTest is null
                                    ? snapshot.SelfTestUpdatedAt : now,
                                TelemetryStale = telemetryStale,
                                StorageStale = storageStale,
                                SelfTestStale = selfTestStale
                            };
                        }
                    }
                }
            }
            catch (Exception error) when (IsDeviceError(error))
            {
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

    private void HandleDisconnect(Exception error)
    {
        lock (deviceGate)
        lock (gate)
        {
            var reconnecting = !commandOutcomeUncertain && Environment.TickCount64 < mscReconnectDeadline &&
                (error is IOException or TimeoutException ||
                    error is InvalidOperationException && error.Message.StartsWith(
                        "PicoForge HID interface not found;", StringComparison.Ordinal));
            if (reconnecting)
                logger.LogInformation(error, "USB-drive mode change: waiting for HID reconnection");
            activeClient = null;
            telemetryReceipt = storageReceipt = selfTestReceipt = 0;
            snapshot = new(false, commandOutcomeUncertain
                    ? "A prior HID command outcome is uncertain; further writes are blocked"
                    : reconnecting ? null : error.Message,
                null, null, null, null, null, null,
                CommandOutcomeUncertain: commandOutcomeUncertain, Reconnecting: reconnecting);
        }
    }

    private static bool IsDeviceError(Exception error) => error is IOException or InvalidOperationException
        or UnauthorizedAccessException or NotSupportedException or TimeoutException;

    private static bool IsStale(long receivedAt, int maximumAgeMs) =>
        receivedAt == 0 || Stopwatch.GetElapsedTime(receivedAt).TotalMilliseconds >= maximumAgeMs;

    public bool SetSelfTest(string action, uint sizeMib)
    {
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null || snapshot.Hardware is null
                    || (snapshot.Hardware.Capabilities & HidProtocol.CapabilitySelfTest) == 0) return false;
                client = activeClient;
            }
            ExecuteCommand(client, () => client.SetSelfTest(action, sizeMib));
            return true;
        }
    }

    private void ExecuteCommand(IHidCommands client, Action command) =>
        ExecuteCommand(client, () => { command(); return true; });

    private T ExecuteCommand<T>(IHidCommands client, Func<T> command)
    {
        if (commandOutcomeUncertain)
            throw new InvalidOperationException(
                "A prior HID command outcome is uncertain; further writes are blocked");
        try
        {
            return command();
        }
        catch
        {
            if (client.CommandOutcomeUncertain)
            {
                commandOutcomeUncertain = true;
                lock (gate)
                    snapshot = snapshot with
                    {
                        Error = "A prior HID command outcome is uncertain; further writes are blocked",
                        CommandOutcomeUncertain = true
                    };
            }
            throw;
        }
    }
}
