using System.Security.Cryptography;
using System.Text;

namespace PicoForge;

public sealed record DashboardBoard(string Id, string ProductName, string? SerialNumber, string DevicePath);
public sealed record DashboardBoardList(DashboardBoard[] Boards, string? DefaultBoardId);

/// <summary>Board discovery plus one lazily started monitor per listed board.</summary>
internal interface IDashboardBoards
{
    DashboardBoardList Boards();
    /// <summary>The board's snapshot, or an empty disconnected one when no board is selected.</summary>
    StatusResponse Snapshot(string? boardId);
    /// <summary>The listed board's started monitor; throws <see cref="KeyNotFoundException"/> otherwise.</summary>
    IDeviceMonitor Monitor(string? boardId);
}

internal sealed class DashboardBoards : IDisposable, IDashboardBoards
{
    private static readonly TimeSpan StaleSerialBoardGracePeriod = TimeSpan.FromSeconds(30);
    private readonly object gate = new();
    private readonly object discoveryGate = new();
    private readonly string? serialNumber;
    private readonly string? devicePath;
    private readonly Func<HidBoard[]> discover;
    private readonly Func<string?, string?, IDeviceMonitor> createMonitor;
    private readonly TimeProvider timeProvider;
    private readonly Dictionary<string, DashboardBoard> known = new();
    private readonly Dictionary<string, DateTimeOffset> lastSeen = new();
    private readonly Dictionary<string, MonitorEntry> monitors = new();
    private bool closed;

    public DashboardBoards(string? serialNumber = null, string? devicePath = null, ILoggerFactory? loggerFactory = null)
        : this(serialNumber, devicePath, PicoForgeHid.Discover,
            (serial, path) => new DeviceMonitor(serial, path, loggerFactory?.CreateLogger<DeviceMonitor>()),
            TimeProvider.System) { }

    internal DashboardBoards(string? serialNumber, string? devicePath, Func<HidBoard[]> discover,
        Func<string?, string?, IDeviceMonitor> createMonitor, TimeProvider? timeProvider = null)
    {
        this.serialNumber = serialNumber;
        this.devicePath = devicePath;
        this.discover = discover;
        this.createMonitor = createMonitor;
        this.timeProvider = timeProvider ?? TimeProvider.System;
    }

    public DashboardBoardList Boards()
    {
        lock (discoveryGate)
        {
            var seen = new HashSet<string>();
            var boards = discover().Select(device =>
            {
                var serial = string.IsNullOrEmpty(device.SerialNumber) ? null : device.SerialNumber;
                var identity = serial is null ? $"path:{device.DevicePath}" : $"serial:{serial}";
                var id = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(identity)));
                if (!seen.Add(id))
                    throw new InvalidOperationException(
                        "duplicate PicoForge board identity; board serials must be unique");
                var productName = string.IsNullOrEmpty(device.ProductName) ? "PicoForge" : device.ProductName;
                return new DashboardBoard(id, productName, serial, device.DevicePath);
            }).ToArray();
            MonitorEntry[] expiredMonitors;
            DashboardBoardList listing;
            lock (gate)
            {
                ObjectDisposedException.ThrowIf(closed, this);
                var now = timeProvider.GetUtcNow();
                foreach (var board in boards)
                {
                    known[board.Id] = board;
                    lastSeen[board.Id] = now;
                }
                var expiredIds = known
                    .Where(entry => !seen.Contains(entry.Key) &&
                        (entry.Value.SerialNumber is null ||
                         now - lastSeen[entry.Key] >= StaleSerialBoardGracePeriod) &&
                        (!monitors.TryGetValue(entry.Key, out var monitor) ||
                         !monitor.Snapshot.CommandOutcomeUncertain))
                    .Select(entry => entry.Key)
                    .ToArray();
                expiredMonitors = expiredIds
                    .Where(monitors.ContainsKey)
                    .Select(id => monitors[id])
                    .ToArray();
                foreach (var id in expiredIds)
                {
                    known.Remove(id);
                    lastSeen.Remove(id);
                    monitors.Remove(id);
                }
                var preferred = boards.FirstOrDefault(board =>
                    (serialNumber is not null && board.SerialNumber == serialNumber)
                    || (devicePath is not null && board.DevicePath == devicePath));
                listing = new DashboardBoardList(boards, preferred?.Id
                    ?? (serialNumber is null && devicePath is null ? boards.FirstOrDefault()?.Id : null));
            }
            // ponytail: An unresolved write keeps this monitor and board entry until shutdown so reconnects cannot
            // silently clear the write block. It may retain one board indefinitely; safe clearing needs firmware
            // recovery.
            foreach (var monitor in expiredMonitors) monitor.Dispose();
            return listing;
        }
    }

    public StatusResponse Snapshot(string? boardId) => boardId is null
        ? new(false, null)
        : Monitor(boardId).Snapshot;

    public IDeviceMonitor Monitor(string? boardId)
    {
        MonitorEntry entry;
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(closed, this);
            if (boardId is null || !known.TryGetValue(boardId, out var board))
                throw new KeyNotFoundException("unknown board; rescan and select a listed board");
            if (!monitors.TryGetValue(boardId, out var current))
            {
                entry = new MonitorEntry(createMonitor(
                    board.SerialNumber, board.SerialNumber is null ? board.DevicePath : null));
                monitors.Add(boardId, entry);
            }
            else entry = current;
        }
        try
        {
            return entry.GetStarted();
        }
        catch
        {
            lock (gate)
                if (monitors.TryGetValue(boardId!, out var current) && ReferenceEquals(current, entry))
                    monitors.Remove(boardId!);
            entry.Dispose();
            throw;
        }
    }

    public void Dispose()
    {
        MonitorEntry[] readers;
        lock (gate)
        {
            if (closed) return;
            closed = true;
            readers = monitors.Values.ToArray();
        }
        foreach (var reader in readers) reader.Dispose();
    }

    private sealed class MonitorEntry(IDeviceMonitor monitor) : IDisposable
    {
        private readonly object gate = new();
        private bool started;
        private bool disposed;

        public StatusResponse Snapshot => monitor.Snapshot;

        public IDeviceMonitor GetStarted()
        {
            lock (gate)
            {
                ObjectDisposedException.ThrowIf(disposed, this);
                if (!started)
                {
                    try { monitor.Start(); }
                    catch
                    {
                        monitor.Dispose();
                        disposed = true;
                        throw;
                    }
                    started = true;
                }
                return monitor;
            }
        }

        public void Dispose()
        {
            lock (gate)
            {
                if (disposed) return;
                disposed = true;
                monitor.Dispose();
            }
        }
    }
}