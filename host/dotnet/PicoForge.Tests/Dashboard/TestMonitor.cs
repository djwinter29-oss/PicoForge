// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

namespace PicoForge.Tests;

/// <summary>An <see cref="IDeviceMonitor"/> whose members throw until a test overrides the ones it needs.</summary>
internal class TestMonitor : IDeviceMonitor
{
    public virtual StatusResponse Snapshot { get; set; } = new(false, null, null, null, null, null);
    public bool Disposed { get; private set; }
    public virtual void Start() { }
    public virtual bool TryRetire() => !Snapshot.CommandOutcomeUncertain;
    public virtual void SetQuarantined(bool value) { }
    public virtual CommandResyncResponse ResyncCommand(bool confirm, CancellationToken cancel = default) =>
        confirm ? new(false) : throw new ApiInputException("Explicit confirm: true is required");
    public virtual void Dispose() => Disposed = true;
    public virtual ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }
    public virtual void SetLed(bool on, CancellationToken cancel = default) => throw new NotSupportedException();
    public virtual MscStatusResponse SetMsc(bool enabled, bool force = false, CancellationToken cancel = default) =>
        throw new NotSupportedException();
    public virtual BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false,
        CancellationToken cancel = default) => throw new NotSupportedException();
    public virtual void Reboot(CancellationToken cancel = default) => throw new NotSupportedException();
    public virtual void SetSelfTest(string action, uint sizeMib, CancellationToken cancel = default) =>
        throw new NotSupportedException();
    public virtual WirelessResponse ReadWireless(CancellationToken cancel = default) =>
        throw new NotSupportedException();
    public virtual WirelessResponse SetWireless(WirelessRequest settings, CancellationToken cancel = default) =>
        throw new NotSupportedException();
    public virtual IReadOnlyList<WirelessNetworkResponse> ScanWireless(CancellationToken cancel = default) =>
        throw new NotSupportedException();
    public virtual WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null,
        CancellationToken cancel = default) => throw new NotSupportedException();
    public virtual void CancelWirelessTest(CancellationToken cancel = default) => throw new NotSupportedException();
    public virtual byte[] TcpCliAuthProof(byte[] nonce, byte[] uid, CancellationToken cancel = default) =>
        throw new NotSupportedException("USB TCP CLI authentication is unavailable");
}

/// <summary>A clock that moves only when a test calls <see cref="Advance"/>.</summary>
internal sealed class ManualTime : TimeProvider
{
    private long ticks = 1;
    private DateTimeOffset now = new(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);
    public override long TimestampFrequency => TimeSpan.TicksPerSecond;
    public override long GetTimestamp() => Interlocked.Read(ref ticks);
    public override DateTimeOffset GetUtcNow() => now;

    public void Advance(TimeSpan by)
    {
        Interlocked.Add(ref ticks, by.Ticks);
        now += by;
    }
}

/// <summary>Serves <paramref name="monitor"/> as board <c>board-a</c>; any other ID is unknown (404).</summary>
internal sealed class OneBoard(IDeviceMonitor monitor) : IDashboardBoards
{
    public DashboardBoardList Boards() => new([new("board-a", "PicoForge", "uid", "/hid/a")], "board-a", []);
    public StatusResponse Snapshot(string? boardId) => Monitor(boardId).Snapshot;
    public IDeviceMonitor Monitor(string? boardId) => boardId is null or "board-a" ? monitor
        : throw new BoardNotFoundException("unknown board; rescan and select a listed board");
}
