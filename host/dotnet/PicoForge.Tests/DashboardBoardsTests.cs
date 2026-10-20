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
}