using System.Diagnostics;
using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.DependencyInjection;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ForcedSaveTests
{
    private static readonly BootSettingsResponse Settings = new(200, 1, 0);
    private static HardwareInfoResponse Hardware(byte capabilities = 0x4c) =>
        new("PicoForge", "RP2350", 2, "0001020304050607", Capabilities: capabilities);

    private static readonly BoardConfigurationResponse Configuration = new(200, 1, 0, 0, 0, [], 0, 0, 0, 0);

    private static DeviceMonitor Attached(IHidCommands commands, StatusResponse status, TimeProvider? time = null)
    {
        var monitor = new DeviceMonitor(() => throw new NotSupportedException(), null, time);
        monitor.Attach(commands, status);
        return monitor;
    }

    private static void AssertRefused(Action command) =>
        Assert.True(Record.Exception(command) is InvalidOperationException or IOException);

    private static StatusResponse Ready(bool enabled = true, byte capabilities = 0x4c) =>
        WithMsc(new(enabled, false, false), capabilities);

    private static StatusResponse WithMsc(MscStatusResponse? msc, byte capabilities = 0x4c) =>
        new(true, null, Hardware(capabilities), null, null,
            new MemoryStatusResponse(1, true, true, true, false, null, null, null, null, null, null) { Msc = msc },
            BoardConfiguration: Configuration);

    private static StatusResponse WirelessReady() => Ready(enabled: false, capabilities: 0xec) with
    {
        SelfTest = new(1, "idle", 0, 0, 0, 0, 0, 0, 0, 0, false, false),
        SelfTestStale = false,
        StorageStale = false
    };

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    public void SaveFlagsPreserveArguments(byte flags)
    {
        var arguments = new byte[64];
        arguments[0] = 200;
        arguments[2] = 1;
        var request = HidProtocol.BuildControlRequest(7, Opcode.SaveSettings, flags, arguments);
        Assert.Equal(69, request.Length);
        Assert.Equal(new byte[] { 20, 1, 7, 5, flags, 200, 0, 1, 0, 0 }, request[..10]);
        Assert.All(request[10..], value => Assert.Equal(0, value));
    }

    [Theory]
    [InlineData(4)]
    [InlineData(8)]
    [InlineData(128)]
    [InlineData(255)]
    public void SaveRejectsReservedFlags(byte flags) =>
        Assert.Throws<ArgumentException>(() =>
            HidProtocol.BuildControlRequest(7, Opcode.SaveSettings, flags, new byte[64]));

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ForcedDisconnectPollsOriginalTransactionWithoutReplayingSet(bool disconnectDuringSet)
    {
        var writes = new List<byte[]>();
        var reads = 0;
        var reopens = 0;
        using var hid = Hid((report, size) =>
        {
            Assert.Equal(HidProtocol.ControlReportId, report);
            Assert.Equal(HidProtocol.ControlFeatureSize, size);
            reads++;
            if (!disconnectDuringSet && reads == 2) throw new IOException("detached");
            return Result(reads < 3 ? (byte)1 : (byte)2);
        }, request =>
        {
            writes.Add(request);
            if (disconnectDuringSet) throw new IOException("detached after SET");
        }, uid =>
        {
            Assert.Equal(Hardware().SerialNumber, uid);
            reopens++;
            if (reopens == 1) throw new IOException("not back yet");
        });

        Assert.Equal(Settings, hid.SetSettings(Settings, true));
        Assert.Single(writes);
        Assert.Equal(new byte[] { 20, 1, 1, 5, 1, 200, 0, 1, 0, 0 }, writes[0][..10]);
        Assert.Equal(69, writes[0].Length);
        Assert.Equal(2, reopens);
        Assert.False(hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData("replaced")]
    [InlineData("opcode")]
    [InlineData("idle")]
    [InlineData("invalid")]
    [InlineData("settings")]
    [InlineData("confirmation")]
    public void InvalidForcedResultRemainsUncertainAndBlocksFurtherWrites(string failure)
    {
        var writes = 0;
        var result = Result(2);
        switch (failure)
        {
            case "replaced": result[2] = 2; break;
            case "opcode": result[3] = 2; result.AsSpan(6).Clear(); break;
            case "idle": result = Result(0); result[2] = result[3] = 0; break;
            case "invalid": result[4] = 7; break;
            case "settings": result[6] = 150; break;
            case "confirmation": result = Result(3); result[5] = 2; break;
        }
        using var hid = Hid((_, _) => result, _ => writes++,
            _ => Assert.Fail("invalid results must not trigger reconnect"));
        Assert.Throws<InvalidDataException>(() => hid.SetSettings(Settings, true));
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Throws<BoardStateException>(() => hid.SetSettings(Settings));
        Assert.Equal(1, writes);
    }

    [Theory]
    [InlineData(3, 3)]
    [InlineData(5, 5)]
    [InlineData(6, 6)]
    public void ForcedRejectionNeverFallsBackToNormalSave(byte status, byte reason)
    {
        var writes = new List<byte[]>();
        var result = Result(status);
        result[5] = reason;
        using var hid = Hid((_, _) => result, writes.Add,
            _ => Assert.Fail("terminal rejection must not reconnect"));
        Assert.Throws<BoardStateException>(() => hid.SetSettings(Settings, true));
        Assert.Single(writes);
        Assert.Equal((byte)1, writes[0][4]);
        Assert.False(hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData(150, 1)]
    [InlineData(200, 0)]
    public void WrongValidSettingsAfterReconnectRemainUncertain(byte clock, byte profile)
    {
        var writes = 0;
        var reads = 0;
        var reopens = 0;
        using var hid = Hid((_, _) =>
        {
            if (++reads == 1) throw new IOException("detached");
            var result = Result(2);
            result[6] = clock;
            result[8] = profile;
            return result;
        }, _ => writes++, uid =>
        {
            Assert.Equal(Hardware().SerialNumber, uid);
            reopens++;
        });
        var error = Assert.Throws<InvalidDataException>(() => hid.SetSettings(Settings, true));
        Assert.Contains("selected next-boot settings", error.Message);
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Throws<BoardStateException>(() => hid.SetSettings(Settings));
        Assert.Equal(1, writes);
        Assert.Equal(1, reopens);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void PendingTimeoutDoesNotReplayAndRemainsUncertain(bool force)
    {
        var writes = new List<byte[]>();
        var time = new ManualTime();
        using var hid = Hid((_, _) =>
        {
            time.Advance(TimeSpan.FromMilliseconds(250));
            return Result(1);
        }, writes.Add, _ => Assert.Fail("a pending response must not trigger reconnect"), time);
        var started = time.GetTimestamp();
        var error = Assert.Throws<TimeoutException>(() => hid.SetSettings(Settings, force));
        Assert.Equal(TimeSpan.FromMilliseconds(force ? 5000 : 1000), time.GetElapsedTime(started));
        Assert.Single(writes);
        Assert.Equal(force ? (byte)1 : (byte)0, writes[0][4]);
        Assert.True(hid.CommandOutcomeUncertain);
        if (force) Assert.Contains("uncertain", error.Message);
    }

    [Fact]
    public void NormalSaveDisconnectDoesNotReconnectOrForce()
    {
        var writes = new List<byte[]>();
        using var hid = Hid((_, _) => throw new IOException("detached"),
            writes.Add, _ => Assert.Fail("normal commands must not reconnect"));
        Assert.Throws<IOException>(() => hid.SetSettings(Settings));
        Assert.Single(writes);
        Assert.Equal((byte)0, writes[0][4]);
        Assert.True(hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void ReconnectTimeoutKeepsOriginalUidAndNeverReplaysSet()
    {
        var writes = 0;
        var reopens = 0;
        using var hid = Hid((_, _) => throw new IOException("detached"),
            _ => writes++, uid =>
            {
                Assert.Equal(Hardware().SerialNumber, uid);
                reopens++;
                throw new IOException("original board unavailable");
            });
        var error = Assert.Throws<TimeoutException>(() => hid.SetSettings(Settings, true));
        Assert.Contains("uncertain", error.Message);
        Assert.Equal(1, writes);
        Assert.True(reopens > 0);
        Assert.True(hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void MonitorGatesForceAndNormalOwnershipWithoutFallback()
    {
        var commands = new Commands();
        using var monitor = new DeviceMonitor();
        foreach (var status in BlockedStatuses())
        {
            monitor.Attach(commands, status);
            AssertRefused(() => monitor.SetSettings(Settings, true));
        }
        monitor.Attach(commands, Ready());
        AssertRefused(() => monitor.SetSettings(Settings));
        Assert.Empty(commands.Forces);
        Assert.Equal(Settings, monitor.SetSettings(Settings, true));
        Assert.Equal(new[] { true }, commands.Forces);
        Assert.False(monitor.Snapshot.Memory!.Msc!.Enabled);

        monitor.Attach(commands, Ready(false, 0x0c));
        Assert.Equal(Settings, monitor.SetSettings(Settings));
        Assert.Equal(new[] { true, false }, commands.Forces);
    }

    [Fact]
    public void MonitorRetainsUncertaintyAfterForcedFailure()
    {
        var commands = new Commands { Failure = new TimeoutException("forced result unavailable") };
        using var monitor = Attached(commands, Ready());
        Assert.Throws<TimeoutException>(() => monitor.SetSettings(Settings, true));
        Assert.True(monitor.Snapshot.CommandOutcomeUncertain);
        Assert.Contains("uncertain", monitor.Snapshot.Error);
        Assert.Throws<BoardStateException>(() => monitor.SetSettings(Settings, true));
        Assert.Equal(new[] { true }, commands.Forces);
    }

    [Theory]
    [InlineData("read", true, false, true)]
    [InlineData("missing", true, false, true)]
    [InlineData("read", true, true, false)]
    [InlineData("read", false, false, false)]
    [InlineData("noop", true, false, false)]
    [InlineData("invalid", true, false, false)]
    [InlineData("permissions", true, false, false)]
    [InlineData("uncertain", true, false, false)]
    public void MonitorOnlyTreatsConfirmedBoundedMscDisconnectAsReconnecting(
        string kind, bool confirmed, bool expired, bool reconnecting)
    {
        var time = new ManualTime();
        var commands = new Commands();
        using var monitor = Attached(commands, Ready(kind == "noop"), time);
        if (confirmed) monitor.SetMsc(true);
        if (expired) time.Advance(TimeSpan.FromSeconds(6));
        if (kind == "uncertain")
        {
            commands.Failure = new TimeoutException("self-test result unavailable");
            Assert.Throws<TimeoutException>(() => monitor.SetSelfTest("start", 1));
        }
        Exception error = kind switch
        {
            "missing" => new IOException("PicoForge HID interface not found; check USB"),
            "invalid" => new InvalidDataException("unexpected HID report"),
            "permissions" => new UnauthorizedAccessException("access denied"),
            _ => new IOException("read error")
        };
        monitor.HandleDisconnect(error);
        Assert.False(monitor.Snapshot.Connected);
        Assert.Equal(reconnecting, monitor.Snapshot.Reconnecting);
        if (kind == "uncertain") Assert.Contains("uncertain", monitor.Snapshot.Error);
        else Assert.Equal(reconnecting ? null : error.Message, monitor.Snapshot.Error);
    }

    [Fact]
    public void MonitorBlocksNormalSaveAndRebootDuringPendingTransitionOrSelfTest()
    {
        var commands = new Commands();
        using var monitor = new DeviceMonitor();
        foreach (var status in new[]
        {
            WithMsc(new(false, true, false)),
            Ready(false) with { SelfTest = new(1, "queued", 1024, 0, 0, 0, 0, 0, 0, 0, false, true) },
            Ready()
        })
        {
            monitor.Attach(commands, status);
            Assert.Throws<BoardStateException>(() => monitor.SetSettings(Settings));
            Assert.Throws<BoardStateException>(() => monitor.Reboot());
        }
        Assert.Empty(commands.Forces);
        Assert.Equal(0, commands.Reboots);
    }

    [Fact]
    public async Task MonitorSurvivesInternalFaultsAndRetries()
    {
        var attempts = 0;
        using var monitor = new DeviceMonitor(() =>
        {
            Interlocked.Increment(ref attempts);
            throw new ArgumentException("parser bug");
        });
        await monitor.StartAsync(CancellationToken.None);
        var deadline = Environment.TickCount64 + 5000;
        while (Volatile.Read(ref attempts) < 2 && Environment.TickCount64 < deadline) await Task.Delay(50);
        await monitor.StopAsync(CancellationToken.None);
        Assert.True(attempts >= 2, "the monitor must keep retrying after a non-device fault");
        Assert.StartsWith("Internal monitor error", monitor.Snapshot.Error);
    }

    [Fact]
    public void SnapshotHidesSectionsThatAgeOutWithoutNewReports()
    {
        var time = new ManualTime();
        var runtime = new RuntimeStatusResponse(1, 1, 200_000_000, true, false, false, null, true, false);
        using var monitor = Attached(new Commands(), Ready() with { Runtime = runtime, TelemetryStale = false }, time);
        Assert.NotNull(monitor.Snapshot.Runtime);
        time.Advance(TimeSpan.FromMilliseconds(800));
        var aged = monitor.Snapshot;
        Assert.Null(aged.Runtime);
        Assert.True(aged.TelemetryStale);
        Assert.NotNull(aged.Memory?.Msc);
        time.Advance(TimeSpan.FromMilliseconds(800));
        Assert.Null(monitor.Snapshot.Memory);
        Assert.True(monitor.Snapshot.StorageStale);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CancelReachesRunningWifiTestWhileOtherCommandsGetBusy(bool testFinishesFirst)
    {
        var commands = new Commands { TestFinishesBeforeCancelCheck = testFinishesFirst };
        using var monitor = Attached(commands, WirelessReady());
        var test = Task.Run(() => monitor.TestWireless("Pico", "secret12"));
        Assert.True(commands.TestStarted.Wait(TimeSpan.FromSeconds(2)));

        var busy = Stopwatch.StartNew();
        Assert.Throws<BoardBusyException>(() => monitor.SetSelfTest("start", 1));
        Assert.InRange(busy.ElapsedMilliseconds, 1500, 5000);
        Assert.Equal(0, commands.SelfTests);

        var cancel = Stopwatch.StartNew();
        monitor.CancelWirelessTest();
        Assert.True(cancel.ElapsedMilliseconds < 1000, "cancel must not wait for the running test");
        if (testFinishesFirst) Assert.Equal(5, (await test).State);
        else await Assert.ThrowsAsync<WirelessTestCancelledException>(() => test);
        Assert.Equal(1, commands.Cancels);
        Assert.False(monitor.Snapshot.CommandOutcomeUncertain);
    }

    [Fact]
    public void AbortedRequestStopsWaitingWithoutSending()
    {
        var commands = new Commands();
        using var monitor = Attached(commands, WirelessReady());
        var test = Task.Run(() => monitor.TestWireless("Pico", "secret12"));
        Assert.True(commands.TestStarted.Wait(TimeSpan.FromSeconds(2)));
        using var aborted = new CancellationTokenSource(TimeSpan.FromMilliseconds(100));
        Assert.Throws<OperationCanceledException>(() => monitor.SetSelfTest("start", 1, aborted.Token));
        Assert.Equal(0, commands.SelfTests);
        monitor.CancelWirelessTest();
        Assert.Throws<AggregateException>(() => test.Wait());
    }

    [Fact]
    public void LedOnDisconnectedBoardIsUnavailableNotConflict()
    {
        using var monitor = Attached(new Commands(), Ready() with { Connected = false });
        Assert.Throws<IOException>(() => monitor.SetLed(true));
    }

    [Fact]
    public void WirelessCancelDeliveryFailureMarksOutcomeUncertain()
    {
        var commands = new Commands
        {
            CancelUncertain = true,
            CancelFailure = new TimeoutException("wireless cancel timed out")
        };
        using var monitor = Attached(commands, WirelessReady());

        Assert.Throws<TimeoutException>(() => monitor.CancelWirelessTest());
        Assert.True(monitor.Snapshot.CommandOutcomeUncertain);
        Assert.Contains("uncertain", monitor.Snapshot.Error);
        Assert.Throws<BoardStateException>(() => monitor.CancelWirelessTest());
    }

    [Fact]
    public async Task HttpRequiresBooleanConfirmationAndForceGates()
    {
        var commands = new Commands();
        using var monitor = Attached(commands, Ready());
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.ConfigureHttpJsonOptions(options => DashboardJson.Configure(options.SerializerOptions));
        builder.Services.AddOpenApi();
        builder.Services.AddSingleton<IDashboardBoards>(new OneBoard(monitor));
        builder.Services.Configure<RouteHandlerOptions>(options => options.ThrowOnBadRequest = true);
        await using var app = builder.Build();
        app.UseApiErrors();
        app.UseSameOriginApiWrites();
        app.MapStatusEndpoints();
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        using (var crossOrigin = new HttpRequestMessage(HttpMethod.Post, "/api/led?board=board-a"))
        {
            crossOrigin.Headers.Add("Origin", "http://evil.example");
            crossOrigin.Content = new StringContent("{\"on\":true}", Encoding.UTF8, "application/json");
            Assert.Equal(HttpStatusCode.Forbidden, (await client.SendAsync(crossOrigin)).StatusCode);
        }

        var outOfRange = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 100, sdcard_profile = 1, radios = 0 });
        Assert.Equal(HttpStatusCode.BadRequest, outOfRange.StatusCode);
        foreach (var invalid in new[] { "null", "1", "\"true\"", "{}", "[]", "" })
        {
            using var body = new StringContent(
                $"{{\"system_clock_mhz\":200,\"sdcard_profile\":1,\"radios\":0,\"confirm_data_loss\":{invalid}}}",
                Encoding.UTF8, "application/json");
            var response = await client.PostAsync("/api/settings?board=board-a", body);
            Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
            Assert.True((await response.Content.ReadFromJsonAsync<JsonElement>()).TryGetProperty("error", out _));
        }
        Assert.Empty(commands.Forces);
        foreach (var status in BlockedStatuses())
        {
            monitor.Attach(commands, status);
            var response = await client.PostAsJsonAsync("/api/settings?board=board-a",
                new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0, confirm_data_loss = true });
            Assert.Equal(status.Connected ? HttpStatusCode.Conflict : HttpStatusCode.ServiceUnavailable,
                response.StatusCode);
            Assert.False(string.IsNullOrEmpty(
                (await response.Content.ReadFromJsonAsync<JsonElement>()).GetProperty("error").GetString()));
        }
        Assert.Empty(commands.Forces);
        monitor.Attach(commands, Ready() with { CommandOutcomeUncertain = true });
        var uncertain = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0, confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.Conflict, uncertain.StatusCode);
        Assert.Empty(commands.Forces);
        monitor.Attach(commands, Ready());
        var forced = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0, confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.OK, forced.StatusCode);
        monitor.Attach(commands, Ready(false));
        var normal = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0 });
        Assert.Equal(HttpStatusCode.OK, normal.StatusCode);
        var unconfirmed = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0, confirm_data_loss = false });
        Assert.Equal(HttpStatusCode.OK, unconfirmed.StatusCode);
        Assert.Equal(new[] { true, false, false }, commands.Forces);
        await app.StopAsync();
    }

    private static StatusResponse[] BlockedStatuses() =>
    [
        Ready(false),
        Ready() with { Connected = false },
        WithMsc(null),
        WithMsc(new(true, true, false)),
        Ready() with { SelfTest = new(1, "writing", 1024, 0, 0, 0, 0, 0, 0, 0, false, true) }
    ];

    private static byte[] Result(byte status)
    {
        var report = new byte[69];
        report[0] = 20;
        report[1] = report[2] = 1;
        report[3] = 5;
        report[4] = status;
        if (status == 2) { report[6] = 200; report[8] = 1; }
        return report;
    }

    private static PicoForgeHid Hid(Func<byte, int, byte[]> read, Action<byte[]> write, Action<string> reopen,
        TimeProvider? time = null) =>
        new(Hardware(), (id, size) => id == 2
            ? [2, 1, 200, 0, 1, 255, 255, 255, 2, 3, 4, 5, 255, 255, 0, 0, 255, 255]
            : read(id, size), write, reopen, time);

    internal sealed class Commands : IHidCommands
    {
        public List<bool> Forces { get; } = [];
        public Exception? Failure { get; set; }
        public Exception? CancelFailure { get; set; }
        public bool CancelUncertain { get; set; }
        public int Reboots { get; private set; }
        public int SelfTests { get; private set; }
        public int Cancels { get; private set; }
        public bool TestFinishesBeforeCancelCheck { get; init; }
        public ManualResetEventSlim TestStarted { get; } = new();
        public bool CommandOutcomeUncertain => Failure is not null || CancelUncertain;
        public ControlCommandReference? UncertainCommand => null;
        public WirelessOperationReference? UncertainWirelessOperation => null;
        public CommandResyncResult? ReconcileOutcome(ControlCommandReference? command,
            WirelessOperationReference? wirelessOperation) => null;
        public InputReport? ReadInputReport(int timeoutMilliseconds = 250) =>
            throw new NotSupportedException();
        public void SetLed(bool on) { }
        public void SetMsc(bool enabled, bool force = false) { }
        public BoardConfigurationResponse ReadConfiguration() => throw new NotSupportedException();
        public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
        {
            Forces.Add(force);
            if (Failure is not null) throw Failure;
            return settings;
        }
        public void Reboot() => Reboots++;
        public void SetSelfTest(string action, uint sizeMib)
        {
            if (Failure is not null) throw Failure;
            SelfTests++;
        }
        public WirelessStatusResponse ReadWireless() => throw new NotSupportedException();
        public byte[] TcpCliAuthProof(byte[] nonce, byte[] uid) => throw new NotSupportedException();
        public WirelessStatusResponse SetWireless(WirelessRequest settings) => throw new NotSupportedException();
        public IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null) =>
            throw new NotSupportedException();
        // Mirrors PicoForgeHid: the test thread sends the cancel itself, or misses it after its last check.
        public WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
            Action<string>? onProgress = null, string? ssidHex = null, CancellationToken cancel = default)
        {
            TestStarted.Set();
            if (!cancel.WaitHandle.WaitOne(TimeSpan.FromSeconds(10))) throw new TimeoutException("never cancelled");
            if (TestFinishesBeforeCancelCheck) return new(5, 5, "192.168.1.2", 0, 1);
            CancelWirelessTest();
            throw new WirelessTestCancelledException();
        }
        public void CancelWirelessTest()
        {
            if (CancelFailure is not null) throw CancelFailure;
            Cancels++;
        }
    }
}
