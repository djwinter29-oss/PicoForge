namespace PicoForge;

internal interface IDeviceMonitor
{
    StatusResponse Snapshot { get; }
    bool SetLed(bool on);
    MscStatusResponse? SetMsc(bool enabled);
    bool SetSelfTest(string action, uint sizeMib);
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private readonly object gate = new();
    private PicoForgeHid? activeClient;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null, null, null, null);

    public StatusResponse Snapshot
    {
        get { lock (gate) return snapshot; }
    }

    public bool SetLed(bool on)
    {
        lock (gate)
        {
            if (activeClient is null || snapshot.BoardStatus?.LedAvailable != true) return false;
            activeClient.SetLed(on);
            snapshot = snapshot with { BoardStatus = snapshot.BoardStatus with { LedOn = on } };
            return true;
        }
    }

    public MscStatusResponse? SetMsc(bool enabled)
    {
        lock (gate)
        {
            if (activeClient is null) return null;
            activeClient.SetMsc(enabled);
            snapshot = snapshot with
            {
                Msc = new MscStatusResponse(snapshot.Msc?.Enabled ?? false, true, false)
            };
            return snapshot.Msc;
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
                lock (gate)
                {
                    activeClient = client;
                    snapshot = new(true, null, hardware, null, null, null, null, null);
                }

                while (!stoppingToken.IsCancellationRequested)
                {
                    lock (gate)
                    {
                        var report = client.ReadInputReport();
                        var runtime = report?.Runtime;
                        var memory = report?.Memory;
                        var msc = report?.Msc;
                        var boardStatus = runtime?.ToBoardStatus() ?? snapshot.BoardStatus;
                        snapshot = new(true, null, hardware, runtime ?? snapshot.Runtime,
                            report is null ? snapshot.UpdatedAt : DateTimeOffset.UtcNow, boardStatus,
                            memory ?? snapshot.Memory, msc ?? snapshot.Msc,
                            report?.SelfTest ?? snapshot.SelfTest);
                    }
                }
            }
            catch (Exception error) when (IsDeviceError(error))
            {
                lock (gate)
                {
                    activeClient = null;
                    snapshot = new(false, error.Message, null, null, null, null, null, null);
                }
            }
            finally
            {
                lock (gate)
                {
                    if (ReferenceEquals(activeClient, client)) activeClient = null;
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
        lock (gate)
        {
            if (activeClient is null || snapshot.SelfTest is null) return false;
            activeClient.SetSelfTest(action, sizeMib);
            return true;
        }
    }
}