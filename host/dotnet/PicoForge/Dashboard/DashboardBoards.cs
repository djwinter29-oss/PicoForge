// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Logging.Abstractions;

namespace PicoForge;

public sealed record DashboardBoard(string Id, string ProductName, string? SerialNumber, string DevicePath);
public sealed record DashboardBoardList(DashboardBoard[] Boards, string? DefaultBoardId, string[] Warnings);

/// <summary>Board discovery plus one lazily started monitor per listed board.</summary>
internal interface IDashboardBoards
{
    DashboardBoardList Boards();
    /// <summary>The board's snapshot, or an empty disconnected one when no board is selected.</summary>
    StatusResponse Snapshot(string? boardId);
    /// <summary>The listed board's started monitor; throws <see cref="KeyNotFoundException"/> otherwise.</summary>
    IDeviceMonitor Monitor(string? boardId);
}

internal sealed class DashboardBoards : IDisposable, IAsyncDisposable, IDashboardBoards
{
    // ponytail: 16 active/retiring owners bound local resources; add managed eviction only for larger fleets.
    private const int MaximumMonitors = 16;
    private static readonly TimeSpan StaleSerialBoardGracePeriod = TimeSpan.FromSeconds(30);
    private readonly object gate = new();
    private readonly object discoveryGate = new();
    private readonly string? serialNumber;
    private readonly string? devicePath;
    private readonly Func<HidBoard[]> discover;
    private readonly Func<string?, string?, IDeviceMonitor> createMonitor;
    private readonly TimeProvider timeProvider;
    private readonly ILogger logger;
    private readonly Dictionary<string, DashboardBoard> known = new();
    private readonly Dictionary<string, DateTimeOffset> lastSeen = new();
    private readonly Dictionary<string, MonitorEntry> monitors = new();
    private readonly HashSet<string> quarantined = new();
    private bool closed;
    private Task? disposal;

    public DashboardBoards(string? serialNumber = null, string? devicePath = null, ILoggerFactory? loggerFactory = null)
        : this(serialNumber, devicePath, PicoForgeHid.Discover,
            (serial, path) => new DeviceMonitor(serial, path, loggerFactory?.CreateLogger<DeviceMonitor>()),
            TimeProvider.System, loggerFactory?.CreateLogger<DashboardBoards>()) { }

    internal DashboardBoards(string? serialNumber, string? devicePath, Func<HidBoard[]> discover,
        Func<string?, string?, IDeviceMonitor> createMonitor, TimeProvider? timeProvider = null, ILogger? logger = null)
    {
        this.serialNumber = serialNumber;
        this.devicePath = devicePath;
        this.discover = discover;
        this.createMonitor = createMonitor;
        this.timeProvider = timeProvider ?? TimeProvider.System;
        this.logger = logger ?? NullLogger.Instance;
    }

    public DashboardBoardList Boards()
    {
        lock (discoveryGate)
        {
            var discovered = discover().Select(device =>
            {
                var serial = string.IsNullOrEmpty(device.SerialNumber) ? null : device.SerialNumber;
                var identity = serial is null ? $"path:{device.DevicePath}" : $"serial:{serial}";
                var id = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(identity)));
                var productName = string.IsNullOrEmpty(device.ProductName) ? "PicoForge" : device.ProductName;
                return new DashboardBoard(id, productName, serial, device.DevicePath);
            }).ToArray();
            var duplicateIds = discovered.GroupBy(board => board.Id)
                .Where(group => group.Count() > 1)
                .Select(group => group.Key)
                .ToHashSet();
            var boards = discovered.Where(board => !duplicateIds.Contains(board.Id)).ToArray();
            var seen = discovered.Select(board => board.Id).ToHashSet();
            var warnings = duplicateIds.Count == 0
                ? Array.Empty<string>()
                : [$"Skipped {duplicateIds.Count} ambiguous board identity group(s); " +
                    "serial numbers or paths must be unique"];
            DashboardBoardList listing;
            lock (gate)
            {
                ObjectDisposedException.ThrowIf(closed, this);
                quarantined.UnionWith(duplicateIds);
                quarantined.ExceptWith(boards.Select(board => board.Id));
                foreach (var (id, entry) in monitors)
                    entry.SetQuarantined(quarantined.Contains(id));
                foreach (var id in monitors.Where(pair => pair.Value.RetirementCompleted)
                    .Select(pair => pair.Key).ToArray())
                    monitors.Remove(id);
                var now = timeProvider.GetUtcNow();
                foreach (var board in boards)
                {
                    known[board.Id] = board;
                    lastSeen[board.Id] = now;
                }
                var expiredIds = known
                    .Where(entry => !seen.Contains(entry.Key) &&
                        (entry.Value.SerialNumber is null ||
                         now - lastSeen[entry.Key] >= StaleSerialBoardGracePeriod))
                    .Select(entry => entry.Key)
                    .ToArray();
                foreach (var id in duplicateIds.Concat(expiredIds).Distinct())
                {
                    if (monitors.TryGetValue(id, out var monitor) && !monitor.TryRetire()) continue;
                    known.Remove(id);
                    lastSeen.Remove(id);
                    quarantined.Remove(id);
                }
                var preferred = boards.FirstOrDefault(board =>
                    (serialNumber is not null && board.SerialNumber == serialNumber)
                    || (devicePath is not null && board.DevicePath == devicePath));
                listing = new DashboardBoardList(boards, preferred?.Id
                    ?? (serialNumber is null && devicePath is null ? boards.FirstOrDefault()?.Id : null), warnings);
            }
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
                throw new BoardNotFoundException("unknown board; rescan and select a listed board");
            if (quarantined.Contains(boardId))
                throw new BoardStateException("Board identity is ambiguous; rescan after removing duplicate identities");
            if (!monitors.TryGetValue(boardId, out var current))
            {
                if (monitors.Count >= MaximumMonitors)
                    throw new BoardStateException("Monitor capacity reached; wait for cleanup or restart the dashboard");
                entry = new MonitorEntry(createMonitor(
                    board.SerialNumber, board.SerialNumber is null ? board.DevicePath : null), logger);
                monitors.Add(boardId, entry);
            }
            else entry = current;
        }
        return entry.GetStarted();
    }

    public ValueTask DisposeAsync()
    {
        lock (gate)
        {
            closed = true;
            disposal ??= Task.WhenAll(monitors.Values.Select(entry => entry.StopAsync()));
            return new(disposal);
        }
    }

    public void Dispose() => DisposeAsync().AsTask().GetAwaiter().GetResult();

    private sealed class MonitorEntry(IDeviceMonitor monitor, ILogger logger)
    {
        private readonly object gate = new();
        private bool started;
        private Task? retirement;

        public bool RetirementCompleted => Volatile.Read(ref retirement)?.IsCompletedSuccessfully == true;

        public void SetQuarantined(bool value) => monitor.SetQuarantined(value);

        public IDeviceMonitor GetStarted()
        {
            lock (gate)
            {
                if (retirement is not null) throw new BoardStateException(
                    "Board monitor is shutting down or cleanup failed; wait for rescan or restart the dashboard");
                if (!started)
                {
                    try { monitor.Start(); }
                    catch
                    {
                        BeginStop();
                        throw;
                    }
                    started = true;
                }
                return monitor;
            }
        }

        public bool TryRetire()
        {
            if (!System.Threading.Monitor.TryEnter(gate)) return false;
            try
            {
                if (retirement is not null) return true;
                if (!monitor.TryRetire()) return false;
                BeginStop();
                return true;
            }
            finally { System.Threading.Monitor.Exit(gate); }
        }

        public Task StopAsync()
        {
            lock (gate) return BeginStop();
        }

        private Task BeginStop() => retirement ??= StopMonitorAsync();

        private async Task StopMonitorAsync()
        {
            try { await monitor.DisposeAsync().ConfigureAwait(false); }
            catch (Exception error)
            {
                logger.LogError(error, "Failed to stop board monitor; retaining ownership until dashboard restart");
                throw;
            }
        }
    }
}