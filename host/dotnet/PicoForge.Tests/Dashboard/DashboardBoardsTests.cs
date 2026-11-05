using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class DashboardBoardsTests
{
    [Theory]
    [InlineData("board-B", null, 1)]
    [InlineData(null, "/hid/a", 0)]
    public void ConfiguredSerialOrPathSelectsTheDefaultBoard(string? serial, string? path, int expected)
    {
        HidBoard[] attached =
        [
            new("/hid/a", "board-A", "PicoForge"),
            new("/hid/b", "board-B", "PicoForge")
        ];
        using var boards = new DashboardBoards(serial, path, () => attached,
            (_, _) => throw new InvalidOperationException("test discovery only"));

        var result = boards.Boards();

        Assert.Equal(result.Boards[expected].Id, result.DefaultBoardId);
    }

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
        var clock = new ManualTime();
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
        Assert.Throws<KeyNotFoundException>(() => boards.Snapshot(id));

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
        Assert.Throws<KeyNotFoundException>(() => boards.Snapshot(id));
        attached = [new("/hid/a", "different-board", "PicoForge")];
        Assert.NotEqual(id, boards.Boards().Boards[0].Id);
    }

    [Fact]
    public void UncertainCommandKeepsMonitorAndWriteBlockAcrossStaleBoardExpiry()
    {
        var clock = new ManualTime();
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

    [Fact]
    public async Task DiscoveryDoesNotBlockSnapshotsForKnownBoards()
    {
        using var discoveryStarted = new ManualResetEventSlim();
        using var releaseDiscovery = new ManualResetEventSlim();
        var blockDiscovery = 0;
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        using var boards = new DashboardBoards(null, null, () =>
        {
            if (Volatile.Read(ref blockDiscovery) != 0)
            {
                discoveryStarted.Set();
                releaseDiscovery.Wait();
            }
            return attached;
        }, (_, _) => new FakeDeviceMonitor());

        var id = boards.Boards().Boards[0].Id;
        boards.Snapshot(id);
        Volatile.Write(ref blockDiscovery, 1);
        var scan = Task.Run(boards.Boards);
        try
        {
            Assert.True(discoveryStarted.Wait(TimeSpan.FromSeconds(2)));
            var snapshot = await Task.Run(() => boards.Snapshot(id)).WaitAsync(TimeSpan.FromSeconds(2));
            Assert.NotNull(snapshot);
        }
        finally
        {
            releaseDiscovery.Set();
        }
        await scan;
    }

    [Fact]
    public void MonitorStartupDoesNotBlockBoardRegistry()
    {
        using var startEntered = new ManualResetEventSlim();
        using var releaseStart = new ManualResetEventSlim();
        var monitor = new FakeDeviceMonitor
        {
            OnStart = () =>
            {
                startEntered.Set();
                releaseStart.Wait();
            }
        };
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        using var boards = new DashboardBoards(null, null, () => attached, (_, _) => monitor);
        var id = boards.Boards().Boards[0].Id;
        StatusResponse? snapshot = null;
        Exception? snapshotError = null;
        using var snapshotFinished = new ManualResetEventSlim();
        var snapshotThread = new Thread(() =>
        {
            try { snapshot = boards.Snapshot(id); }
            catch (Exception error) { snapshotError = error; }
            finally { snapshotFinished.Set(); }
        });
        snapshotThread.Start();
        Thread? listingThread = null;
        try
        {
            Assert.True(startEntered.Wait(TimeSpan.FromSeconds(2)));
            DashboardBoardList? listing = null;
            Exception? listingError = null;
            using var listingFinished = new ManualResetEventSlim();
            listingThread = new Thread(() =>
            {
                try { listing = boards.Boards(); }
                catch (Exception error) { listingError = error; }
                finally { listingFinished.Set(); }
            });
            listingThread.Start();
            Assert.True(listingFinished.Wait(TimeSpan.FromSeconds(2)));
            Assert.Null(listingError);
            Assert.Single(listing!.Boards);
        }
        finally
        {
            releaseStart.Set();
        }
        Assert.True(snapshotFinished.Wait(TimeSpan.FromSeconds(2)));
        snapshotThread.Join();
        listingThread?.Join();
        Assert.Null(snapshotError);
        Assert.NotNull(snapshot);
    }

    [Fact]
    public async Task MonitorStartReturnsBeforeSynchronousHidInitializationCompletes()
    {
        using var readEntered = new ManualResetEventSlim();
        using var releaseRead = new ManualResetEventSlim();
        var hid = new PicoForgeHid(
            new HardwareInfoResponse("PicoForge", "RP2350", 2, "board-A"),
            (_, _) =>
            {
                readEntered.Set();
                releaseRead.Wait();
                throw new IOException("test disconnect");
            }, _ => { }, _ => { });
        using var monitor = new DeviceMonitor(() => hid);

        var startTask = Task.Factory.StartNew(
            () => monitor.StartAsync(CancellationToken.None),
            CancellationToken.None,
            TaskCreationOptions.LongRunning,
            TaskScheduler.Default).Unwrap();
        try
        {
            await startTask.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.True(readEntered.Wait(TimeSpan.FromSeconds(2)));
        }
        finally
        {
            releaseRead.Set();
        }
        await monitor.StopAsync(CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(2));
    }


    private sealed class FakeDeviceMonitor : TestMonitor
    {
        public Action? OnStart { get; init; }

        public override void Start() => OnStart?.Invoke();
    }
}