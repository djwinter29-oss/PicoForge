namespace PicoForge.Host;

internal interface IDeviceMonitor
{
    StatusResponse Snapshot { get; }
}

internal sealed class DeviceMonitor : BackgroundService, IDeviceMonitor
{
    private readonly object gate = new();
    private StatusResponse snapshot = new(false, "Waiting for PicoForge HID", null, null, null);

    public StatusResponse Snapshot
    {
        get { lock (gate) return snapshot; }
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
                lock (gate) snapshot = new(true, null, hardware, null, null);

                while (!stoppingToken.IsCancellationRequested)
                {
                    var runtime = client.ReadStatus();
                    if (runtime is not null)
                    {
                        lock (gate) snapshot = new(true, null, hardware, runtime, DateTimeOffset.UtcNow);
                    }
                }
            }
            catch (Exception error) when (IsDeviceError(error))
            {
                lock (gate) snapshot = new(false, error.Message, null, null, null);
            }
            finally
            {
                client?.Dispose();
            }

            try { await Task.Delay(TimeSpan.FromSeconds(1), stoppingToken); }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested) { break; }
        }
    }

    private static bool IsDeviceError(Exception error) => error is IOException or InvalidOperationException
        or UnauthorizedAccessException or NotSupportedException or TimeoutException;
}