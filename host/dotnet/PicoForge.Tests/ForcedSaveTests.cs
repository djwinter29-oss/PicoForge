using System.Net;
using System.Net.Http.Json;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ForcedSaveTests
{
    private static readonly BootSettingsResponse Settings = new(200, 1);
    private static HardwareInfoResponse Hardware(byte capabilities = 0x4c) =>
        new("PicoForge", "RP2350", 2, "0001020304050607", Capabilities: capabilities);

    private static StatusResponse Ready(bool enabled = true, byte capabilities = 0x4c) =>
        new(true, null, Hardware(capabilities), null, null, null, null,
            new(enabled, false, false), Settings: Settings);

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void SaveFlagsPreserveArguments(byte flags)
    {
        Assert.Equal(new byte[] { 20, 1, 7, 6, flags, 200, 0, 1, 0, 0 },
            HidProtocol.BuildControlRequest(7, 6, flags, [200, 0, 1, 0, 0]));
    }

    [Theory]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(128)]
    [InlineData(255)]
    public void SaveRejectsReservedFlags(byte flags) =>
        Assert.Throws<InvalidOperationException>(() =>
            HidProtocol.BuildControlRequest(7, 6, flags, [200, 0, 1, 0, 0]));

    [Theory]
    [InlineData(0x0c)]
    [InlineData(0x40)]
    public void ForcedSaveRequiresBothCapabilities(byte capabilities)
    {
        var writes = 0;
        using var hid = new PicoForgeHid(Hardware(capabilities), (_, _) => Result(2),
            _ => writes++, _ => Assert.Fail("must not reconnect"));
        Assert.Throws<NotSupportedException>(() => hid.SetSettings(Settings, true));
        Assert.Equal(0, writes);
        Assert.False(hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ForcedDisconnectPollsOriginalTransactionWithoutReplayingSet(bool disconnectDuringSet)
    {
        var writes = new List<byte[]>();
        var reads = 0;
        var reopens = 0;
        using var hid = new PicoForgeHid(Hardware(), (report, size) =>
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
        Assert.Equal(new byte[] { 20, 1, 1, 6, 1, 200, 0, 1, 0, 0 }, writes[0]);
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
        using var hid = new PicoForgeHid(Hardware(), (_, _) => result, _ => writes++,
            _ => Assert.Fail("invalid results must not trigger reconnect"));
        Assert.Throws<InvalidOperationException>(() => hid.SetSettings(Settings, true));
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Throws<InvalidOperationException>(() => hid.SetSettings(Settings));
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
        using var hid = new PicoForgeHid(Hardware(), (_, _) => result, writes.Add,
            _ => Assert.Fail("terminal rejection must not reconnect"));
        Assert.Throws<InvalidOperationException>(() => hid.SetSettings(Settings, true));
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
        using var hid = new PicoForgeHid(Hardware(), (_, _) =>
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
        var error = Assert.Throws<InvalidOperationException>(() => hid.SetSettings(Settings, true));
        Assert.Contains("selected next-boot settings", error.Message);
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Throws<InvalidOperationException>(() => hid.SetSettings(Settings));
        Assert.Equal(1, writes);
        Assert.Equal(1, reopens);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void PendingTimeoutDoesNotReplayAndRemainsUncertain(bool force)
    {
        var writes = new List<byte[]>();
        using var hid = new PicoForgeHid(Hardware(), (_, _) => Result(1), writes.Add,
            _ => Assert.Fail("a pending response must not trigger reconnect"));
        var started = Environment.TickCount64;
        var error = Assert.Throws<TimeoutException>(() => hid.SetSettings(Settings, force));
        var elapsed = Environment.TickCount64 - started;
        Assert.InRange(elapsed, force ? 5000 : 1000, force ? 7000 : 3000);
        Assert.Single(writes);
        Assert.Equal(force ? (byte)1 : (byte)0, writes[0][4]);
        Assert.True(hid.CommandOutcomeUncertain);
        if (force) Assert.Contains("uncertain", error.Message);
    }

    [Fact]
    public void NormalSaveDisconnectDoesNotReconnectOrForce()
    {
        var writes = new List<byte[]>();
        using var hid = new PicoForgeHid(Hardware(), (_, _) => throw new IOException("detached"),
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
        using var hid = new PicoForgeHid(Hardware(), (_, _) => throw new IOException("detached"),
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
        typeof(DeviceMonitor).GetField("activeClient", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(monitor, commands);
        var snapshot = typeof(DeviceMonitor).GetField("snapshot", BindingFlags.Instance | BindingFlags.NonPublic)!;
        foreach (var status in BlockedStatuses())
        {
            snapshot.SetValue(monitor, status);
            Assert.Null(monitor.SetSettings(Settings, true));
        }
        snapshot.SetValue(monitor, Ready());
        Assert.Null(monitor.SetSettings(Settings));
        Assert.Empty(commands.Forces);
        Assert.Equal(Settings, monitor.SetSettings(Settings, true));
        Assert.Equal(new[] { true }, commands.Forces);
        Assert.False(monitor.Snapshot.Msc!.Enabled);

        snapshot.SetValue(monitor, Ready(false, 0x0c));
        Assert.Equal(Settings, monitor.SetSettings(Settings));
        Assert.Equal(new[] { true, false }, commands.Forces);
    }

    [Fact]
    public void MonitorRetainsUncertaintyAfterForcedFailure()
    {
        var commands = new Commands { Failure = new TimeoutException("forced result unavailable") };
        using var monitor = new DeviceMonitor();
        typeof(DeviceMonitor).GetField("activeClient", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(monitor, commands);
        typeof(DeviceMonitor).GetField("snapshot", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(monitor, Ready());
        Assert.Throws<TimeoutException>(() => monitor.SetSettings(Settings, true));
        Assert.True(monitor.Snapshot.CommandOutcomeUncertain);
        Assert.Contains("uncertain", monitor.Snapshot.Error);
        Assert.Throws<InvalidOperationException>(() => monitor.SetSettings(Settings, true));
        Assert.Equal(new[] { true }, commands.Forces);
    }

    [Theory]
    [InlineData("read", true, false, true)]
    [InlineData("missing", true, false, true)]
    [InlineData("read", true, true, false)]
    [InlineData("read", false, false, false)]
    [InlineData("invalid", true, false, false)]
    [InlineData("permissions", true, false, false)]
    [InlineData("uncertain", true, false, false)]
    public void MonitorOnlyTreatsConfirmedBoundedMscDisconnectAsReconnecting(
        string kind, bool confirmed, bool expired, bool reconnecting)
    {
        using var monitor = new DeviceMonitor();
        var fields = BindingFlags.Instance | BindingFlags.NonPublic;
        typeof(DeviceMonitor).GetField("activeClient", fields)!.SetValue(monitor, new Commands());
        typeof(DeviceMonitor).GetField("snapshot", fields)!.SetValue(monitor, Ready(false));
        if (confirmed) Assert.NotNull(monitor.SetMsc(true));
        if (expired)
            typeof(DeviceMonitor).GetField("mscReconnectDeadline", fields)!.SetValue(monitor, Environment.TickCount64 - 1);
        if (kind == "uncertain")
            typeof(DeviceMonitor).GetField("commandOutcomeUncertain", fields)!.SetValue(monitor, true);
        Exception error = kind switch
        {
            "missing" => new InvalidOperationException("PicoForge HID interface not found; check USB"),
            "invalid" => new InvalidOperationException("unexpected HID report"),
            "permissions" => new UnauthorizedAccessException("access denied"),
            _ => new IOException("read error")
        };
        typeof(DeviceMonitor).GetMethod("HandleDisconnect", fields)!.Invoke(monitor, [error]);
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
        typeof(DeviceMonitor).GetField("activeClient", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(monitor, commands);
        var snapshot = typeof(DeviceMonitor).GetField("snapshot", BindingFlags.Instance | BindingFlags.NonPublic)!;
        foreach (var status in new[]
        {
            Ready(false) with { Msc = new(false, true, false) },
            Ready(false) with { SelfTest = new(1, "queued", 1024, 0, 0, 0, 0, 0, 0, 0, 0, false, true) },
            Ready()
        })
        {
            snapshot.SetValue(monitor, status);
            Assert.Null(monitor.SetSettings(Settings));
            Assert.False(monitor.Reboot());
        }
        Assert.Empty(commands.Forces);
        Assert.Equal(0, commands.Reboots);
    }

    [Fact]
    public async Task HttpRequiresBooleanConfirmationAndForceGates()
    {
        var dashboard = new Dashboard();
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.ConfigureHttpJsonOptions(options =>
            options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
        builder.Services.AddOpenApi();
        builder.Services.AddSingleton<IDashboardBoards>(dashboard);
        await using var app = builder.Build();
        app.MapDeviceStatus();
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };

        foreach (var invalid in new[] { "null", "1", "\"true\"", "{}", "[]", "" })
        {
            using var body = new StringContent(
                $"{{\"system_clock_mhz\":200,\"sdcard_profile\":1,\"confirm_data_loss\":{invalid}}}",
                Encoding.UTF8, "application/json");
            var response = await client.PostAsync("/api/settings?board=board-a", body);
            Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
        }
        Assert.Empty(dashboard.Forces);
        foreach (var status in BlockedStatuses())
        {
            dashboard.Status = status;
            var response = await client.PostAsJsonAsync("/api/settings?board=board-a",
                new { system_clock_mhz = 200, sdcard_profile = 1, confirm_data_loss = true });
            Assert.Equal(HttpStatusCode.Conflict, response.StatusCode);
        }
        Assert.Empty(dashboard.Forces);
        dashboard.Status = Ready() with { CommandOutcomeUncertain = true };
        var uncertain = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.Conflict, uncertain.StatusCode);
        Assert.Empty(dashboard.Forces);
        dashboard.Status = Ready();
        var forced = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.OK, forced.StatusCode);
        dashboard.Status = Ready(false);
        var normal = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1 });
        Assert.Equal(HttpStatusCode.OK, normal.StatusCode);
        var unconfirmed = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, confirm_data_loss = false });
        Assert.Equal(HttpStatusCode.OK, unconfirmed.StatusCode);
        Assert.Equal(new[] { true, false, false }, dashboard.Forces);
        await app.StopAsync();
    }

    private static StatusResponse[] BlockedStatuses() =>
    [
        Ready(false),
        Ready(capabilities: 0x0c),
        Ready() with { Connected = false },
        Ready() with { Msc = null },
        Ready() with { Msc = new(true, true, false) },
        Ready() with { SelfTest = new(1, "writing", 1024, 0, 0, 0, 0, 0, 0, 0, 0, false, true) }
    ];

    private static byte[] Result(byte status) =>
        [20, 1, 1, 6, status, 0, status == 2 ? (byte)200 : (byte)0, 0,
            status == 2 ? (byte)1 : (byte)0, 0];

    private sealed class Commands : IHidCommands
    {
        public List<bool> Forces { get; } = [];
        public Exception? Failure { get; set; }
        public int Reboots { get; private set; }
        public bool CommandOutcomeUncertain => Failure is not null;
        public void SetLed(bool on) { }
        public void SetMsc(bool enabled, bool force = false) { }
        public BoardConfigurationResponse ReadConfiguration() => throw new NotSupportedException();
        public BootSettingsResponse ReadSettings() => Settings;
        public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
        {
            Forces.Add(force);
            if (Failure is not null) throw Failure;
            return settings;
        }
        public void Reboot() => Reboots++;
        public void SetSelfTest(string action, uint sizeMib) { }
    }

    private sealed class Dashboard : IDashboardBoards
    {
        public StatusResponse Status { get; set; } = Ready();
        public List<bool> Forces { get; } = [];
        public DashboardBoardList Boards() => new([], null);
        public StatusResponse Snapshot(string? boardId) => Status;
        public bool SetLed(string? boardId, bool on) => true;
        public MscStatusResponse? SetMsc(string? boardId, bool enabled, bool force = false) =>
            new(enabled, false, false);
        public BootSettingsResponse? SetSettings(string? boardId, BootSettingsResponse settings, bool force = false)
        {
            Forces.Add(force);
            return settings;
        }
        public bool Reboot(string? boardId) => true;
        public bool SetSelfTest(string? boardId, string action, uint sizeMib) => true;
    }
}
