// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

using System.Buffers.Binary;
using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text.Json;
using HidSharp;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;

namespace PicoForge.Tests;

public sealed class WirelessLifecycleTests
{
    [Fact]
    public async Task TestingAndCleanupInvalidatePreviousCachedCandidates()
    {
        using var wire = new Wire { FailCancel = false };
        using var hid = new PicoForgeHid(wire, () => []);
        await using var monitor = new DeviceMonitor(() => throw new InvalidOperationException("No worker expected"),
            time: new ManualTime());
        var old = hid.ReadWireless() with { TestGeneration = 255, Ssid = "previous-candidate" };
        monitor.Attach(hid, Ready(hid) with { Wireless = old });
        var test = monitor.TestWireless("Pico", "password");
        Assert.Equal((byte)1, test.TestGeneration);
        Assert.NotNull(test.TestId);
        Assert.Null(monitor.Snapshot.Wireless);
        monitor.Attach(hid, Ready(hid) with { Wireless = hid.ReadWireless() });
        monitor.CancelWirelessTest();
        Assert.Null(monitor.Snapshot.Wireless);
        Assert.False(monitor.Snapshot.WirelessCleanupRequired);
        Assert.Equal(new byte[] { 6, 7, 9, 10 }, wire.Writes.Select(request => request[3]));
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task FailedCancellationRequiresExplicitCleanupWithoutReplaying(bool activeCancel, bool unknown)
    {
        using var wire = new Wire { ActiveTest = activeCancel, LoseCancelResult = unknown };
        using var hid = new PicoForgeHid(wire, () => []);
        await using var monitor = new DeviceMonitor(() => throw new InvalidOperationException("No worker expected"),
            time: new ManualTime());
        var ready = Ready(hid);
        monitor.Attach(hid, ready);
        wire.Cancel = () => monitor.CancelWirelessTest();

        if (unknown) Assert.Throws<IOException>(() => monitor.TestWireless("Pico", "password"));
        else Assert.Throws<BoardStateException>(() => monitor.TestWireless("Pico", "password"));
        Assert.Equal(new byte[] { 6, 7, 9, 10 }, wire.Writes.Select(request => request[3]));
        Assert.Equal(unknown, monitor.Snapshot.CommandOutcomeUncertain);
        AssertCleanupRequired(monitor, true);
        Assert.False(monitor.TryRetire());
        var writes = wire.Writes.Count;
        if (unknown)
        {
            Assert.Throws<BoardStateException>(() => monitor.ResyncCommand(true));
            Assert.Throws<BoardStateException>(() => monitor.CancelWirelessTest());
            wire.LoseCancelResult = false;
            wire.ReplaceCancelResult = true;
            Assert.Throws<BoardStateException>(() => monitor.ResyncCommand(true));
            Assert.True(monitor.Snapshot.CommandOutcomeUncertain);
            wire.ReplaceCancelResult = false;
        }
        Assert.False(monitor.ResyncCommand(true).Uncertain);
        AssertCleanupRequired(monitor, true);
        Assert.Equal(writes, wire.Writes.Count);
        foreach (var command in new Action[]
        {
            () => monitor.SetLed(true), () => monitor.SetMsc(true), () => monitor.SetSettings(new(200, 1, 0)),
            () => monitor.Reboot(), () => monitor.SetSelfTest("start", 1), () => monitor.SetSelfTest("cancel", 1),
            () => monitor.ScanWireless(), () => monitor.TestWireless("Pico", "password"),
            () => monitor.SetWireless(new()), () => monitor.TcpCliAuthProof(new byte[16], new byte[8])
        })
            Assert.Contains("Clear", Assert.Throws<BoardStateException>(command).Message);
        Assert.Equal(writes, wire.Writes.Count);
        Assert.Throws<BoardStateException>(() => monitor.CancelWirelessTest());
        Assert.Equal(writes + 1, wire.Writes.Count);
        AssertCleanupRequired(monitor, true);

        monitor.HandleDisconnect(new IOException("unplugged"));
        AssertCleanupRequired(monitor, true);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task SuccessfulCancelDoesNotReplayOrDependOnTheCancelledTestGeneration(bool activeCancel, bool unknown)
    {
        using var wire = new Wire { ActiveTest = activeCancel, FailCancel = false, LoseCancelResult = unknown };
        using var hid = new PicoForgeHid(wire, () => []);
        await using var monitor = new DeviceMonitor(() => throw new InvalidOperationException("No worker expected"),
            time: new ManualTime());
        monitor.Attach(hid, Ready(hid));
        wire.Cancel = () => monitor.CancelWirelessTest();
        if (unknown) Assert.Throws<IOException>(() => monitor.TestWireless("Pico", "password"));
        else if (activeCancel) Assert.Throws<WirelessTestCancelledException>(() => monitor.TestWireless("Pico", "password"));
        else monitor.TestWireless("Pico", "password");
        Assert.Equal(unknown, monitor.Snapshot.CommandOutcomeUncertain);
        AssertCleanupRequired(monitor, unknown);
        if (unknown)
        {
            Assert.Equal(Opcode.WifiCancel, hid.UncertainCommand?.Opcode);
            Assert.Null(hid.UncertainWirelessOperation);
            wire.LoseCancelResult = false;
            var reconciled = monitor.ResyncCommand(true);
            Assert.False(reconciled.Uncertain);
            Assert.Equal((byte)Opcode.WifiCancel, reconciled.ControlResult?.Opcode);
            Assert.Equal("completed", reconciled.ControlResult?.Status);
        }
        AssertCleanupRequired(monitor, false);
        monitor.SetLed(true);
        Assert.Equal(new byte[] { 6, 7, 9, 10, 1 }, wire.Writes.Select(request => request[3]));
    }

    [Fact]
    public async Task KnownCleanupObligationSurvivesActualWorkerReconnection()
    {
        using var first = new Wire();
        using var second = new Wire { FailCancel = false };
        var opened = 0;
        await using var monitor = new DeviceMonitor(() =>
            new PicoForgeHid(Interlocked.Increment(ref opened) == 1 ? first : second, () => []));
        monitor.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        while (monitor.Snapshot.Memory is null) await Task.Delay(10, timeout.Token);
        Assert.Throws<BoardStateException>(() => monitor.CancelWirelessTest());
        first.Disconnected = true;
        while (Volatile.Read(ref opened) < 2 || monitor.Snapshot.Memory is null)
            await Task.Delay(10, timeout.Token);
        AssertCleanupRequired(monitor, true);
        Assert.False(monitor.TryRetire());
        Assert.Empty(second.Writes);
        Assert.Contains("Clear", Assert.Throws<BoardStateException>(() => monitor.SetLed(true)).Message);
        monitor.CancelWirelessTest();
        AssertCleanupRequired(monitor, false);
        Assert.Equal((byte)Opcode.WifiCancel, Assert.Single(second.Writes)[3]);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    public async Task CompletedSavePublishesLifecycleEvenWhenConfirmationFails(bool failed, bool mismatch)
    {
        using var wire = new Wire { FailSave = failed, MismatchSave = mismatch };
        using var hid = new PicoForgeHid(wire, () => []);
        await using var monitor = new DeviceMonitor(() => throw new InvalidOperationException("No worker expected"),
            time: new ManualTime());
        monitor.Attach(hid, Ready(hid));
        if (failed) Assert.Throws<BoardStateException>(() => monitor.SetSettings(new(200, 1, 0)));
        else if (mismatch) Assert.Throws<InvalidDataException>(() => monitor.SetSettings(new(200, 1, 0)));
        else monitor.SetSettings(new(200, 1, 0));
        Assert.Equal(!failed, monitor.Snapshot.RebootPending);
        Assert.Equal(mismatch, monitor.Snapshot.CommandOutcomeUncertain);
        Assert.Empty(hid.LastCommandResult!.ResultData);
        Assert.Single(wire.Writes);
    }

    [Fact]
    public async Task CleanupGateAndRetryUseProductionHttpWithoutDiallingTheConsole()
    {
        using var wire = new Wire();
        using var hid = new PicoForgeHid(wire, () => []);
        await using var monitor = new DeviceMonitor(() => throw new InvalidOperationException("No worker expected"),
            time: new ManualTime());
        monitor.Attach(hid, Ready(hid) with
        {
            Wireless = hid.ReadWireless() with { WifiState = WifiState.Connected, Ipv4 = "127.0.0.1", TcpCliStarted = true }
        });
        Assert.Throws<BoardStateException>(() => monitor.CancelWirelessTest());

        using var listener = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        listener.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        listener.Listen(1);
        using var bridge = new ConsoleBridge(null, ((IPEndPoint)listener.LocalEndPoint!).Port, true);
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton<IDashboardBoards>(new OneBoard(monitor));
        builder.Services.AddSingleton(bridge);
        await using var app = await DashboardApplication.BuildAsync(builder);
        await app.StartAsync();
        using var http = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        var status = await http.GetFromJsonAsync<JsonElement>("/api/status");
        Assert.True(status.GetProperty("wireless_cleanup_required").GetBoolean());
        var writes = wire.Writes.Count;
        foreach (var (path, body) in new (string, object)[]
        {
            ("/api/led", new { on = true }),
            ("/api/console", new { }),
            ("/api/wireless/scan", new { }),
            ("/api/wireless/test", new { ssid = "Pico", password = "password" })
        })
        {
            using var refused = await http.PostAsJsonAsync(path, body);
            Assert.Equal(HttpStatusCode.Conflict, refused.StatusCode);
            Assert.Contains("clear", (await refused.Content.ReadFromJsonAsync<JsonElement>())
                .GetProperty("error").GetString(), StringComparison.OrdinalIgnoreCase);
        }
        Assert.False(listener.Poll(0, SelectMode.SelectRead));
        Assert.Equal(writes, wire.Writes.Count);
        wire.FailCancel = false;
        using var cancelled = await http.PostAsync("/api/wireless/cancel", null);
        Assert.Equal(HttpStatusCode.OK, cancelled.StatusCode);
        AssertCleanupRequired(monitor, false);
        monitor.SetLed(true);
        Assert.Equal(new byte[] { 10, 10, 1 }, wire.Writes.Select(request => request[3]));
        await app.StopAsync();
    }

    private static void AssertCleanupRequired(DeviceMonitor monitor, bool expected) =>
        Assert.Equal(expected, JsonSerializer.SerializeToElement(monitor.Snapshot, DashboardJson.Options)
            .GetProperty("wireless_cleanup_required").GetBoolean());

    private static StatusResponse Ready(PicoForgeHid hid) => new(true, null, hid.ReadInfo(),
        new(1, 0, 200_000_000, true, false, false, null, true, false),
        Memory: new MemoryStatusResponse(1, true, true, true, false, null, null, null, null, null, null)
            { Msc = new(false, false, false) }, BoardConfiguration: hid.ReadConfiguration(),
        Wireless: hid.ReadWireless());

    /// <summary>Native transport substitute; command, monitor and HTTP behavior remain production code.</summary>
    private sealed class Wire : HidDevice, IDisposable
    {
        private readonly Stream stream;
        private byte[] result = new byte[69];
        private readonly byte[] wireless = new byte[49];
        public List<byte[]> Writes { get; } = [];
        public bool ActiveTest { get; init; }
        public bool FailCancel { get; set; } = true;
        public bool LoseCancelResult { get; set; }
        public bool ReplaceCancelResult { get; set; }
        public volatile bool Disconnected;
        public bool FailSave { get; init; }
        public bool MismatchSave { get; init; }
        public Action? Cancel { get; set; }
        public Wire()
        {
            stream = new(this);
            wireless[0] = 12;
            wireless[1] = 1;
            wireless[5] = 3;
        }
        public override int ProductID => HidProtocol.ProductId;
        public override int VendorID => HidProtocol.VendorId;
        public override int ReleaseNumberBcd => 0;
        public override string DevicePath => "/fake/wireless";
        public override string GetFileSystemName() => DevicePath;
        public override int GetMaxInputReportLength() =>
            Math.Max(HidProtocol.StoragePayloadSize, HidProtocol.SelfTestPayloadSize) + 1;
        public override int GetMaxOutputReportLength() => 0;
        public override int GetMaxFeatureReportLength() => HidProtocol.ControlFeatureSize;
        protected override DeviceStream OpenDeviceDirectly(OpenConfiguration configuration) => stream;
        public void Dispose() => stream.Dispose();

        private byte[] Read(byte id)
        {
            if (id == 1)
            {
                var identity = new byte[HidProtocol.IdentityFeatureSize];
                identity[0] = identity[1] = 1;
                BinaryPrimitives.WriteUInt16LittleEndian(identity.AsSpan(2), 2);
                for (var index = 0; index < 8; index++) identity[index + 4] = (byte)index;
                identity[15] = identity[16] = 2;
                identity[18] = 0x8d;
                BinaryPrimitives.WriteUInt32LittleEndian(identity.AsSpan(19), 520192);
                return identity;
            }
            if (id == 2) return [2, 1, 200, 0, 1, 255, 255, 255, 2, 3, 4, 5, 255, 255, 0x38, 0, 255, 255];
            if (id == 12)
            {
                if (Writes.LastOrDefault()?[3] == 9) Cancel?.Invoke();
                return wireless;
            }
            if (id != 20) throw new InvalidDataException("Unexpected report");
            if (result[3] == 10 && LoseCancelResult) throw new IOException("cancel result unavailable");
            var report = result.ToArray();
            if (result[3] == 10 && ReplaceCancelResult) report[2]++;
            return report;
        }

        private void Write(byte[] request)
        {
            Writes.Add(request.ToArray());
            result = new byte[69];
            result[0] = 20;
            result[1] = 1;
            result[2] = request[2];
            result[3] = request[3];
            result[4] = 2;
            switch (request[3])
            {
                case 5:
                    if (FailSave) { result[4] = 6; result[5] = 6; }
                    else if (!MismatchSave) request.AsSpan(5, 4).CopyTo(result.AsSpan(6));
                    break;
                case 6: wireless[5] |= 16; break;
                case 7: wireless[5] |= 32; break;
                case 9:
                    wireless[47] = 1;
                    wireless[2] = ActiveTest ? (byte)4 : (byte)5;
                    if (!ActiveTest) wireless[5] |= 128;
                    break;
                case 10:
                    if (FailCancel) { result[4] = 6; result[5] = 6; }
                    else { wireless[5] &= 15; wireless[2] = 0; }
                    break;
            }
        }

        private sealed class Stream(Wire owner) : HidStream(owner)
        {
            public override int ReadTimeout { get; set; }
            public override int WriteTimeout { get; set; }
            public override void GetFeature(byte[] buffer, int offset, int count) =>
                owner.Read(buffer[offset]).CopyTo(buffer, offset);
            public override void SetFeature(byte[] buffer, int offset, int count) =>
                owner.Write(buffer.AsSpan(offset, count).ToArray());
            public override int Read(byte[] buffer, int offset, int count)
            {
                if (owner.Disconnected) throw new IOException("unplugged");
                var report = new byte[HidProtocol.StoragePayloadSize + 1];
                report[0] = 10;
                report[1] = report[3] = 1;
                report.CopyTo(buffer, offset);
                return report.Length;
            }
            public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        }
    }
}
