namespace PicoForge.Tests;

/// <summary>An <see cref="IDeviceMonitor"/> whose members throw until a test overrides the ones it needs.</summary>
internal class TestMonitor : IDeviceMonitor
{
    public virtual StatusResponse Snapshot { get; set; } = new(false, null, null, null, null, null, null, null);
    public bool Disposed { get; private set; }
    public virtual Task StartAsync(CancellationToken cancellationToken) => Task.CompletedTask;
    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
    public void Dispose() => Disposed = true;
    public virtual void SetLed(bool on) => throw new NotSupportedException();
    public virtual MscStatusResponse SetMsc(bool enabled, bool force = false) => throw new NotSupportedException();
    public virtual BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false) =>
        throw new NotSupportedException();
    public virtual void Reboot() => throw new NotSupportedException();
    public virtual void SetSelfTest(string action, uint sizeMib) => throw new NotSupportedException();
    public virtual WirelessResponse ReadWireless() => throw new NotSupportedException();
    public virtual WirelessResponse SetWireless(WirelessRequest settings) => throw new NotSupportedException();
    public virtual IReadOnlyList<WirelessNetworkResponse> ScanWireless() => throw new NotSupportedException();
    public virtual WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null) =>
        throw new NotSupportedException();
    public virtual void CancelWirelessTest() => throw new NotSupportedException();
}

/// <summary>Serves <paramref name="monitor"/> as board <c>board-a</c>; any other ID is unknown (404).</summary>
internal sealed class OneBoard(IDeviceMonitor monitor) : IDashboardBoards
{
    public DashboardBoardList Boards() => new([new("board-a", "PicoForge", "uid", "/hid/a")], "board-a");
    public StatusResponse Snapshot(string? boardId) => Monitor(boardId).Snapshot;
    public IDeviceMonitor Monitor(string? boardId) => boardId is null or "board-a" ? monitor
        : throw new KeyNotFoundException("unknown board; rescan and select a listed board");
}
