namespace PicoForge;

internal interface IDeviceMonitor
{
    StatusResponse Snapshot { get; }
    bool SetLed(bool on);
    MscStatusResponse? SetMsc(bool enabled, bool force = false);
    BootSettingsResponse? SetSettings(BootSettingsResponse settings);
    bool SetSelfTest(string action, uint sizeMib);
}

/// <summary>HID command writes used by <see cref="DeviceMonitor"/>; allows tests to block a write.</summary>
internal interface IHidCommands
{
    void SetLed(bool on);
    void SetMsc(bool enabled, bool force = false);
    BootSettingsResponse? ReadSettings();
    BootSettingsResponse SetSettings(BootSettingsResponse settings);
    void SetSelfTest(string action, uint sizeMib);
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private readonly object gate = new();
    // Handle operations always acquire deviceGate before the snapshot gate.
    private readonly object deviceGate = new();
    private IHidCommands? activeClient;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null, null, null, null);

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
        HidProtocol.BuildSettingsReport(settings);
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
            lock (gate)
            {
                snapshot = snapshot with { Settings = saved };
                return saved;
            }
        }
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            PicoForgeHid? client = null;
            try
            {
                client = new PicoForgeHid();
                var hardware = client.ReadInfo();
                var settings = client.ReadSettings();
                lock (deviceGate)
                lock (gate)
                {
                    activeClient = client;
                    snapshot = new(true, null, hardware, null, null, null, null, null, Settings: settings);
                }

                while (!stoppingToken.IsCancellationRequested)
                {
                    lock (deviceGate)
                    {
                        var report = client.ReadInputReport();
                        lock (gate)
                        {
                            var runtime = report?.Runtime;
                            var memory = report?.Memory;
                            var msc = report?.Msc;
                            var boardStatus = runtime?.ToBoardStatus() ?? snapshot.BoardStatus;
                            snapshot = new(true, null, hardware, runtime ?? snapshot.Runtime,
                                report is null ? snapshot.UpdatedAt : DateTimeOffset.UtcNow, boardStatus,
                                memory ?? snapshot.Memory, msc ?? snapshot.Msc,
                                report?.SelfTest ?? snapshot.SelfTest,
                                report?.FilesystemFormat ?? snapshot.FilesystemFormat, snapshot.Settings);
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

    public bool SetSelfTest(string action, uint sizeMib)
    {
        lock (deviceGate)
        {
            IHidCommands? client;
            lock (gate)
            {
                if (activeClient is null || snapshot.SelfTest is null) return false;
                client = activeClient;
            }
            client.SetSelfTest(action, sizeMib);
            return true;
        }
    }
}
