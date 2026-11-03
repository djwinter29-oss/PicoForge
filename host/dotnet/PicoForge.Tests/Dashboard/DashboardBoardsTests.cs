using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class DashboardBoardsTests
{
    [Fact]
    public void DiscoveryIsLazyAndBoardIdsSurvivePathChangesWhenSerialExists()
    {
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge"), new("/hid/b", "board-B", "")];
        var opened = new List<(string? Serial, string? Path)>();
        using var boards = new DashboardBoards(null, null, () => attached, (serial, path) =>
        {
            opened.Add((serial, path));
            throw new InvalidOperationException("test discovery only");
        });

        var listing = boards.Boards();
        Assert.Empty(opened);
        Assert.Equal("board-A", listing.Boards[0].SerialNumber);
        Assert.Equal("PicoForge", listing.Boards[1].ProductName);
        Assert.Equal(listing.Boards[0].Id, listing.DefaultBoardId);
        Assert.Throws<InvalidOperationException>(() => boards.Snapshot(listing.Boards[0].Id));
        Assert.Equal(("board-A", null), opened[0]);

        attached = [new("/hid/new-a", "board-A", "PicoForge")];
        var rescanned = boards.Boards();
        Assert.Equal(listing.Boards[0].Id, rescanned.Boards[0].Id);
    }

    [Fact]
    public void PathFallbackUsesThePathAndDuplicateSerialsAreRejected()
    {
        HidBoard[] attached = [new("/hid/a", null, "PicoForge")];
        using var boards = new DashboardBoards(null, "/hid/a", () => attached,
            (_, _) => throw new InvalidOperationException("test discovery only"));
        var listing = boards.Boards();
        Assert.Equal(listing.Boards[0].Id, listing.DefaultBoardId);
        Assert.Null(listing.Boards[0].SerialNumber);

        attached = [new("/hid/a", "duplicate", "PicoForge"), new("/hid/b", "duplicate", "PicoForge")];
        Assert.Throws<InvalidOperationException>(() => boards.Boards());
    }

    [Fact]
    public void SerialBackedMonitorsSurviveBriefDisconnectAndExpireAfterGracePeriod()
    {
        var clock = new ManualTimeProvider();
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        var created = new List<FakeDeviceMonitor>();
        using var boards = new DashboardBoards(null, null, () => attached, (_, _) =>
        {
            var monitor = new FakeDeviceMonitor();
            created.Add(monitor);
            return monitor;
        }, clock);

        var id = boards.Boards().Boards[0].Id;
        boards.Snapshot(id);
        attached = [];
        clock.Advance(TimeSpan.FromSeconds(29));
        boards.Boards();

        Assert.False(created[0].Disposed);
        Assert.Equal(created[0].Snapshot, boards.Snapshot(id));

        clock.Advance(TimeSpan.FromSeconds(1));
        boards.Boards();
        Assert.True(created[0].Disposed);
        Assert.Throws<InvalidOperationException>(() => boards.Snapshot(id));

        attached = [new("/hid/new-a", "board-A", "PicoForge")];
        Assert.Equal(id, boards.Boards().Boards[0].Id);
        boards.Snapshot(id);
        Assert.Equal(2, created.Count);
    }

    [Fact]
    public void PathOnlyMonitorExpiresOnFirstSuccessfulScanWhereDeviceIsAbsent()
    {
        HidBoard[] attached = [new("/hid/a", null, "PicoForge")];
        var created = new List<FakeDeviceMonitor>();
        using var boards = new DashboardBoards(null, null, () => attached, (_, _) =>
        {
            var monitor = new FakeDeviceMonitor();
            created.Add(monitor);
            return monitor;
        });

        var id = boards.Boards().Boards[0].Id;
        boards.Snapshot(id);
        attached = [];
        boards.Boards();

        Assert.True(created[0].Disposed);
        Assert.Throws<InvalidOperationException>(() => boards.Snapshot(id));
        attached = [new("/hid/a", "different-board", "PicoForge")];
        Assert.NotEqual(id, boards.Boards().Boards[0].Id);
    }

    [Fact]
    public void UncertainCommandKeepsMonitorAndWriteBlockAcrossStaleBoardExpiry()
    {
        var clock = new ManualTimeProvider();
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        var monitor = new FakeDeviceMonitor
        {
            Snapshot = new(false, null, null, null, null, null, null, null, CommandOutcomeUncertain: true)
        };
        using var boards = new DashboardBoards(null, null, () => attached, (_, _) => monitor, clock);

        var id = boards.Boards().Boards[0].Id;
        boards.Snapshot(id);
        attached = [];
        clock.Advance(TimeSpan.FromMinutes(1));
        boards.Boards();

        Assert.False(monitor.Disposed);
        Assert.True(boards.Snapshot(id).CommandOutcomeUncertain);
    }

    private sealed class ManualTimeProvider : TimeProvider
    {
        private DateTimeOffset now = DateTimeOffset.UnixEpoch;

        public override DateTimeOffset GetUtcNow() => now;

        public void Advance(TimeSpan amount) => now += amount;
    }

    private sealed class FakeDeviceMonitor : IDeviceMonitor
    {
        public bool Disposed { get; private set; }
        public StatusResponse Snapshot { get; set; } = new(false, null, null, null, null, null, null, null);

        public Task StartAsync(CancellationToken cancellationToken) => Task.CompletedTask;
        public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
        public void Dispose() => Disposed = true;
        public bool SetLed(bool on) => throw new NotSupportedException();
        public MscStatusResponse? SetMsc(bool enabled, bool force = false) => throw new NotSupportedException();
        public BootSettingsResponse? SetSettings(BootSettingsResponse settings, bool force = false) =>
            throw new NotSupportedException();
        public bool Reboot() => throw new NotSupportedException();
        public bool SetSelfTest(string action, uint sizeMib) => throw new NotSupportedException();
        public WirelessResponse ReadWireless() => throw new NotSupportedException();
        public WirelessResponse SetWireless(WirelessRequest settings) => throw new NotSupportedException();
        public IReadOnlyList<WirelessNetworkResponse> ScanWireless() => throw new NotSupportedException();
        public WirelessOperationResponse TestWireless(string ssid, string password) =>
            throw new NotSupportedException();
        public bool CancelWirelessTest() => throw new NotSupportedException();
    }
}