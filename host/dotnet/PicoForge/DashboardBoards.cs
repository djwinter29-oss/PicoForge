using System.Security.Cryptography;
using System.Text;

namespace PicoForge;

public sealed record DashboardBoard(string Id, string ProductName, string? SerialNumber, string DevicePath);
public sealed record DashboardBoardList(DashboardBoard[] Boards, string? DefaultBoardId);

internal interface IDashboardBoards
{
    DashboardBoardList Boards();
    StatusResponse Snapshot(string? boardId);
    bool SetLed(string? boardId, bool on);
    MscStatusResponse? SetMsc(string? boardId, bool enabled, bool force = false);
    BootSettingsResponse? SetSettings(string? boardId, BootSettingsResponse settings);
    bool Reboot(string? boardId);
    bool SetSelfTest(string? boardId, string action, uint sizeMib);
}

internal sealed class DashboardBoards : IDisposable, IDashboardBoards
{
    private readonly object gate = new();
    private readonly string? serialNumber;
    private readonly string? devicePath;
    private readonly Func<HidBoard[]> discover;
    private readonly Func<string?, string?, DeviceMonitor> createMonitor;
    private readonly Dictionary<string, DashboardBoard> known = new();
    private readonly Dictionary<string, DeviceMonitor> monitors = new();
    private bool closed;

    public DashboardBoards(string? serialNumber = null, string? devicePath = null)
        : this(serialNumber, devicePath, PicoForgeHid.Discover,
            (serial, path) => new DeviceMonitor(serial, path)) { }

    internal DashboardBoards(string? serialNumber, string? devicePath, Func<HidBoard[]> discover,
        Func<string?, string?, DeviceMonitor> createMonitor)
    {
        this.serialNumber = serialNumber;
        this.devicePath = devicePath;
        this.discover = discover;
        this.createMonitor = createMonitor;
    }

    public DashboardBoardList Boards()
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(closed, this);
            var seen = new HashSet<string>();
            var boards = discover().Select(device =>
            {
                var serial = string.IsNullOrEmpty(device.SerialNumber) ? null : device.SerialNumber;
                var identity = serial is null ? $"path:{device.DevicePath}" : $"serial:{serial}";
                var id = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(identity)));
                if (!seen.Add(id))
                    throw new InvalidOperationException("duplicate PicoForge board identity; board serials must be unique");
                var board = new DashboardBoard(id, string.IsNullOrEmpty(device.ProductName) ? "PicoForge" : device.ProductName,
                    serial, device.DevicePath);
                known[id] = board;
                return board;
            }).ToArray();
            var preferred = boards.FirstOrDefault(board =>
                (serialNumber is not null && board.SerialNumber == serialNumber)
                || (devicePath is not null && board.DevicePath == devicePath));
            return new DashboardBoardList(boards, preferred?.Id
                ?? (serialNumber is null && devicePath is null ? boards.FirstOrDefault()?.Id : null));
        }
    }

    public StatusResponse Snapshot(string? boardId) => boardId is null
        ? new(false, null, null, null, null, null, null, null)
        : Reader(boardId).Snapshot;

    public bool SetLed(string? boardId, bool on) => Reader(boardId).SetLed(on);

    public MscStatusResponse? SetMsc(string? boardId, bool enabled, bool force = false) =>
        Reader(boardId).SetMsc(enabled, force);
    public BootSettingsResponse? SetSettings(string? boardId, BootSettingsResponse settings) =>
        Reader(boardId).SetSettings(settings);
    public bool Reboot(string? boardId) => Reader(boardId).Reboot();
    public bool SetSelfTest(string? boardId, string action, uint sizeMib) =>
        Reader(boardId).SetSelfTest(action, sizeMib);

    private DeviceMonitor Reader(string? boardId)
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(closed, this);
            if (boardId is null || !known.TryGetValue(boardId, out var board))
                throw new InvalidOperationException("unknown board; rescan and select a listed board");
            if (!monitors.TryGetValue(boardId, out var monitor))
            {
                monitor = createMonitor(board.SerialNumber, board.SerialNumber is null ? board.DevicePath : null);
                try { monitor.StartAsync(CancellationToken.None).GetAwaiter().GetResult(); }
                catch
                {
                    monitor.Dispose();
                    throw;
                }
                monitors.Add(boardId, monitor);
            }
            return monitor;
        }
    }

    public void Dispose()
    {
        DeviceMonitor[] readers;
        lock (gate)
        {
            if (closed) return;
            closed = true;
            readers = monitors.Values.ToArray();
        }
        foreach (var reader in readers) reader.Dispose();
    }
}