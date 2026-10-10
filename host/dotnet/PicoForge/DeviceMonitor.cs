namespace PicoForge;

internal interface IDeviceMonitor
{
    StatusResponse Snapshot { get; }
    bool SetLed(bool on);
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private readonly object gate = new();
    private PicoForgeHid? activeClient;
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null, null);

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
            snapshot = snapshot with { BoardStatus = activeClient.ReadBoardStatus() };
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
                client = new PicoForgeHid();
                var hardware = client.ReadInfo();
                var boardStatus = client.ReadBoardStatus();
                lock (gate)
                {
                    activeClient = client;
                    snapshot = new(true, null, hardware, null, null, boardStatus);
                }

                while (!stoppingToken.IsCancellationRequested)
                {
                    lock (gate)
                    {
                        var runtime = client.ReadStatus();
                        boardStatus = client.ReadBoardStatus();
                        snapshot = new(true, null, hardware, runtime ?? snapshot.Runtime,
                            runtime is null ? snapshot.UpdatedAt : DateTimeOffset.UtcNow, boardStatus);
                    }
                }
            }
            catch (Exception error) when (IsDeviceError(error))
            {
                lock (gate)
                {
                    activeClient = null;
                    snapshot = new(false, error.Message, null, null, null, null);
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
}