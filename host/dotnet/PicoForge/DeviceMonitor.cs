using System.Diagnostics;

namespace PicoForge;

internal interface IDeviceMonitor
{
    StatusResponse Snapshot { get; }
    bool SetLed(bool on);
    MscStatusResponse? SetMsc(bool enabled, bool force = false);
    BootSettingsResponse? SetSettings(BootSettingsResponse settings);
    bool Reboot();
    bool SetSelfTest(string action, uint sizeMib);
}

/// <summary>HID command writes used by <see cref="DeviceMonitor"/>; allows tests to block a write.</summary>
internal interface IHidCommands
{
    void SetLed(bool on);
    void SetMsc(bool enabled, bool force = false);
    BoardConfigurationResponse ReadConfiguration();
    BootSettingsResponse ReadSettings();
    BootSettingsResponse SetSettings(BootSettingsResponse settings);
    void Reboot();
    void SetSelfTest(string action, uint sizeMib);
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private readonly Func<PicoForgeHid> createClient;
    private readonly object gate = new();
    // Handle operations always acquire deviceGate before the snapshot gate.
    private readonly object deviceGate = new();
    private IHidCommands? activeClient;
    private long telemetryReceipt;
    private long storageReceipt;
    private long selfTestReceipt;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null, null, null, null);

    public DeviceMonitor() : this(null, null) { }

    public DeviceMonitor(string? serialNumber, string? devicePath)
    {
        createClient = () => new PicoForgeHid(serialNumber, devicePath);
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
            client.SetLed(on);
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
            client.SetMsc(enabled, force);
            lock (gate)
            {
                snapshot = snapshot with
                {
                    Msc = new MscStatusResponse(snapshot.Msc?.Enabled ?? false, true, false,
                        snapshot.Msc?.ForceReturnSupported ?? false)
                };
                return snapshot.Msc;
            }
        }
    }

    public BootSettingsResponse? SetSettings(BootSettingsResponse settings)
    {
        if (settings.SystemClockMhz is not (0 or 150 or 200 or 250) || settings.SdCardProfile is < 0 or > 1)
            throw new ArgumentOutOfRangeException(nameof(settings));
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null || snapshot.Settings is null || snapshot.Msc is null ||
                    snapshot.Msc.Enabled || snapshot.Msc.TransitionPending || snapshot.SelfTest?.Active == true)
                    return null;
                client = activeClient;
            }
            var saved = client.SetSettings(settings);
            var configuration = client.ReadConfiguration();
            lock (gate)
            {
                snapshot = snapshot with
                {
                    Settings = saved,
                    BoardConfiguration = configuration
                };
                return saved;
            }
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
            client.Reboot();
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
                    snapshot = new(true, null, hardware, null, null, null, null, null,
                        Settings: settings, BoardConfiguration: configuration);
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
                            var telemetryStale = IsStale(telemetryReceipt, 750);
                            var storageStale = IsStale(storageReceipt, 1500);
                            var selfTestStale = IsStale(selfTestReceipt, 1500);
                            var runtime = telemetryStale ? null : report?.Runtime ?? snapshot.Runtime;
                            var memory = storageStale ? null : report?.Memory ?? snapshot.Memory;
                            var msc = storageStale ? null : report?.Msc ?? snapshot.Msc;
                            snapshot = snapshot with
                            {
                                Connected = true,
                                Error = null,
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
                lock (deviceGate)
                lock (gate)
                {
                    activeClient = null;
                    telemetryReceipt = storageReceipt = selfTestReceipt = 0;
                    snapshot = new(false, error.Message, null, null, null, null, null, null);
                }
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
            client.SetSelfTest(action, sizeMib);
            return true;
        }
    }
}
