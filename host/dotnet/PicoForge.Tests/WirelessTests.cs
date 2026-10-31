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

public sealed class WirelessTests
{
    private static WirelessRequest Settings() => new()
    {
        WifiEnabled = true, BleEnabled = true, Ssid = "Pico網", ConfirmReboot = true, Board = Board
    };

    private static readonly BootSettingsResponse Board = new(200, 1, 3);
    private const string BoardJson = "\"board\":{\"system_clock_mhz\":200,\"sdcard_profile\":1,\"radios\":3}";

    /// <summary>Report 2 readback for an SPI board without an LED; the default matches <see cref="Board"/>.</summary>
    private static byte[] Configuration(byte radios = 3)
    {
        var report = ProtocolTests.ConfigurationReport();
        report[14] = (byte)(report[14] & ~0x02 | radios << 3);
        report[16] = byte.MaxValue;
        report[12] = report[13] = byte.MaxValue;
        return report;
    }

    private static byte[] Report(byte status = 2, byte transaction = 1)
    {
        var report = new byte[49];
        new byte[] { 30, 1, 3, status, transaction, 2, 192, 168, 1, 2 }.CopyTo(report, 0);
        Encoding.UTF8.GetBytes("Pico網").CopyTo(report, 10);
        report[43] = report[44] = 1;
        return report;
    }

    private static byte[] OperationResult(byte state = 2, byte transaction = 1)
    {
        var report = new byte[221];
        report[0] = 35;
        report[1] = 1;
        report[2] = transaction;
        report[3] = state;
        return report;
    }

    private static HardwareInfoResponse Hardware(byte capabilities = 0x88) =>
        new("PicoForge", "RP2040", 1, "0001020304050607", BoardModel: 4, Capabilities: capabilities);

    private static StatusResponse Ready() =>
        new(true, null, Hardware(), null, null, null, null, new(false, false, false),
            StorageStale: false, SelfTestStale: false);

    [Fact]
    public void BuilderUsesExactOffsetsAndOmittedCredentialsArePreserved()
    {
        var report = HidProtocol.BuildWirelessRequest(Settings(), 255);
        Assert.Equal(137, report.Length);
        Assert.Equal(new byte[] { 31, 2, 255, 3, 3 }, report[..5]);
        Assert.Equal(Encoding.UTF8.GetBytes("Pico網"), report[5..12]);
        Assert.All(report[38..134], value => Assert.Equal(0, value));
        Assert.Equal(new byte[] { 200, 0, 1 }, report[134..]);
        var request = Settings();
        request.Password = "secret12";
        request.BleKey = new string('a', 64);
        report = HidProtocol.BuildWirelessRequest(request, 1);
        Assert.Equal(0, report[4]);
        Assert.Equal(Encoding.ASCII.GetBytes("secret12\0"), report[38..47]);
        Assert.All(report[102..134], value => Assert.Equal(0xaa, value));
        Assert.Equal(new byte[] { 200, 0, 1 }, report[134..]);
        request.WifiEnabled = false;
        Assert.Throws<ArgumentException>(() => HidProtocol.BuildWirelessRequest(request, 2));
    }

    [Fact]
    public void ScanAndTestCommandsHaveBoundedPayloadsAndSanitizedResults()
    {
        var scan = HidProtocol.BuildWirelessCommand(7, 1);
        Assert.Equal(102, scan.Length);
        Assert.Equal(new byte[] { 34, 1, 1, 7, 0, 0, 0 }, scan[..7]);
        var test = HidProtocol.BuildWirelessCommand(8, 3, ssid: "Pico網", password: "secret12");
        Assert.Equal(Encoding.UTF8.GetBytes("Pico網"), test[7..14]);
        Assert.Equal(Encoding.ASCII.GetBytes("secret12"), test[39..47]);
        Assert.Throws<ArgumentException>(() => HidProtocol.BuildWirelessCommand(1, 3, ssid: "", password: ""));

        var result = OperationResult();
        result[5] = 1;
        result[7] = 4;
        Encoding.UTF8.GetBytes("test").CopyTo(result, 8);
        result[40] = unchecked((byte)-42);
        result[41] = 1;
        var decoded = HidProtocol.ParseWirelessOperation(result);
        Assert.Single(decoded.Networks);
        Assert.Equal(new WirelessNetworkResponse("test", -42, true), decoded.Networks[0]);
        Assert.DoesNotContain("secret12", JsonSerializer.Serialize(decoded));
        result[41] = 2;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessOperation(result));
    }

    [Theory]
    [InlineData("\"wifi_enabled\":1")]
    [InlineData("\"ble_enabled\":null")]
    [InlineData("\"ssid\":\"\"")]
    [InlineData("\"ssid\":\"\\u0000\"")]
    [InlineData("\"ssid\":\"ééééééééééééééééé\"")]
    [InlineData("\"password\":null")]
    [InlineData("\"password\":\"secret\\n12\"")]
    [InlineData("\"password\":\"short\"")]
    [InlineData("\"password\":\"secret網12\"")]
    [InlineData("\"ble_key\":null")]
    [InlineData("\"ble_key\":\"00\"")]
    [InlineData("\"ble_key\":\"0000000000000000000000000000000000000000000000000000000000000000\"")]
    [InlineData("\"confirm_reboot\":false")]
    [InlineData("\"force\":true")]
    [InlineData("\"board\":null")]
    [InlineData("\"board\":{\"sdcard_profile\":1}")]
    [InlineData("\"board\":{\"system_clock_mhz\":175,\"sdcard_profile\":1}")]
    [InlineData("\"board\":{\"system_clock_mhz\":200,\"sdcard_profile\":1,\"radios\":4}")]
    public void InvalidInputDoesNotEchoSecrets(string field)
    {
        var board = field.StartsWith("\"board\"") ? "" : BoardJson + ",";
        var json = "{\"wifi_enabled\":true,\"ble_enabled\":true,\"ssid\":\"Pico\",\"confirm_reboot\":true," + board + field + "}";
        using var document = JsonDocument.Parse(json);
        var error = Assert.Throws<ArgumentException>(() => WirelessRequest.Parse(document.RootElement));
        Assert.DoesNotContain("secret", error.Message);
    }

    [Fact]
    public void ParserReturnsOnlySanitizedStatusAndRejectsMalformedReports()
    {
        var result = HidProtocol.ParseWirelessStatus(Report());
        Assert.Equal("Pico網", result.Ssid);
        Assert.Equal("192.168.1.2", result.Ipv4);
        Assert.Equal("connected", result.WifiState);
        var json = JsonSerializer.Serialize(result);
        Assert.DoesNotContain("\"Password\":", json);
        Assert.DoesNotContain("\"BleKey\":", json);
        foreach (var (offset, value) in new (int, byte)[]
                 { (1, 2), (2, 4), (3, 7), (5, 4), (43, 2), (44, 2), (45, 4), (48, 1) })
        {
            var report = Report();
            report[offset] = value;
            Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(report));
        }
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(Report()[..48]));
        var noNul = Report();
        noNul.AsSpan(10, 33).Fill(65);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(noNul));
        var badUtf8 = Report();
        badUtf8[10] = 255;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(badUtf8));
    }

    [Fact]
    public void ProvisionSendsOnceAndPollsOnlyThirtyForTheMatchingTransaction()
    {
        var reports = new Queue<byte[]>([Report(0, 0), Report(2, 255), Report(1), Report()]);
        var writes = new List<byte[]>();
        using var hid = new PicoForgeHid(Hardware(), (id, size) =>
        {
            if (id == 2) return Configuration();
            Assert.Equal(30, id);
            Assert.Equal(49, size);
            return reports.Dequeue();
        }, writes.Add, _ => Assert.Fail("Wireless provisioning must not reconnect or replay"));
        Assert.Equal("completed", hid.SetWireless(Settings()).Status);
        Assert.Single(writes);
        Assert.Equal(31, writes[0][0]);
        Assert.False(hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData(3, "invalid")]
    [InlineData(4, "unsupported")]
    [InlineData(5, "busy")]
    [InlineData(6, "failed")]
    public void ExplicitTerminalResultsAreNotRetried(byte code, string name)
    {
        var reads = 0;
        var writes = 0;
        using var hid = new PicoForgeHid(Hardware(), (_, _) => ++reads == 1 ? Report(0, 0) : Report(code),
            _ => writes++, _ => Assert.Fail("No reconnect"));
        var error = Assert.Throws<InvalidOperationException>(() => hid.SetWireless(Settings()));
        Assert.Contains(name, error.Message);
        Assert.Equal(1, writes);
        Assert.False(hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void WirelessOperationFailureIsKnownButDeliveryFailureBlocksReplay()
    {
        using var rejected = new PicoForgeHid(Hardware(), (id, _) =>
            id == 30 ? Report(0, 0) : OperationResult(6, 1),
            _ => { }, _ => Assert.Fail("No reconnect"));
        Assert.Contains("Could not connect",
            Assert.Throws<InvalidOperationException>(() => rejected.TestWireless("Pico網", "secret12")).Message);
        Assert.False(rejected.CommandOutcomeUncertain);

        var writes = 0;
        using var disconnected = new PicoForgeHid(Hardware(), (_, _) => Report(0, 0), _ =>
        {
            writes++;
            throw new IOException("detached");
        }, _ => Assert.Fail("No reconnect"));
        Assert.Throws<IOException>(() => disconnected.ScanWireless());
        Assert.True(disconnected.CommandOutcomeUncertain);
        Assert.Throws<InvalidOperationException>(() => disconnected.ScanWireless());
        Assert.Equal(1, writes);
    }

    [Theory]
    [InlineData(2, 1)]
    [InlineData(2, 2)]
    [InlineData(10, 88)]
    [InlineData(43, 0)]
    [InlineData(44, 0)]
    public void CompletedMismatchRemainsUncertainAndBlocksFurtherWrites(int offset, byte value)
    {
        foreach (var supplied in new[] { false, true })
        {
            var report = Report();
            report[offset] = value;
            var reads = 0;
            var writes = 0;
            var settings = Settings();
            if (supplied)
            {
                settings.Password = "secret12";
                settings.BleKey = new string('a', 64);
            }
            using var hid = new PicoForgeHid(Hardware(), (_, _) => ++reads == 1 ? Report(0, 0) : report,
                _ => writes++, _ => Assert.Fail("No reconnect"));
            var error = Assert.Throws<InvalidOperationException>(() => hid.SetWireless(settings));
            Assert.Contains("uncertain", error.Message);
            Assert.DoesNotContain("secret12", error.Message);
            Assert.True(hid.CommandOutcomeUncertain);
            Assert.Throws<InvalidOperationException>(() => hid.SetWireless(Settings()));
            Assert.Equal(1, writes);
        }
    }

    [Fact]
    public void BoardSettingsReadbackMismatchRemainsUncertain()
    {
        var reads = 0;
        using var hid = new PicoForgeHid(Hardware(), (id, _) => id == 2 ? Configuration(0)
            : ++reads == 1 ? Report(0, 0) : Report(), _ => { }, _ => Assert.Fail("No reconnect"));
        var error = Assert.Throws<InvalidOperationException>(() => hid.SetWireless(Settings()));
        Assert.Contains("uncertain", error.Message);
        Assert.True(hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData(43)]
    [InlineData(44)]
    public void OmittedCredentialsMustPreserveAbsentConfiguration(int offset)
    {
        var before = Report(0, 0);
        before[43] = before[44] = 0;
        var after = (byte[])before.Clone();
        after[3] = 2;
        after[4] = 1;
        after[offset] = 1;
        var reads = 0;
        using var hid = new PicoForgeHid(Hardware(), (_, _) => ++reads == 1 ? before : after,
            _ => { }, _ => Assert.Fail("No reconnect"));
        Assert.Throws<InvalidOperationException>(() => hid.SetWireless(Settings()));
        Assert.True(hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void DisconnectOrTimeoutBlocksFurtherWritesWithoutReplay(bool disconnect)
    {
        var reads = 0;
        var writes = 0;
        using var hid = new PicoForgeHid(Hardware(), (_, _) =>
        {
            if (++reads == 1) return Report(0, 0);
            if (disconnect) throw new IOException("detached");
            return Report(1);
        }, _ => writes++, _ => Assert.Fail("No reconnect"));
        if (disconnect) Assert.Throws<IOException>(() => hid.SetWireless(Settings()));
        else Assert.Throws<TimeoutException>(() => hid.SetWireless(Settings()));
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Throws<InvalidOperationException>(() => hid.SetWireless(Settings()));
        Assert.Equal(1, writes);
    }

    [Fact]
    public void BusyAndOldFirmwareNeverReceiveProvisioningWrites()
    {
        var writes = 0;
        using var old = new PicoForgeHid(Hardware(8), (_, _) => Report(), _ => writes++, _ => { });
        Assert.Throws<NotSupportedException>(() => old.SetWireless(Settings()));
        using var busy = new PicoForgeHid(Hardware(), (_, _) => Report(1), _ => writes++, _ => { });
        Assert.Throws<InvalidOperationException>(() => busy.SetWireless(Settings()));
        Assert.Equal(0, writes);
    }

    [Fact]
    public void MonitorRequiresIdleFirmwareOwnershipAndMarksRebootPending()
    {
        using var monitor = new DeviceMonitor();
        var commands = new Commands();
        typeof(DeviceMonitor).GetField("activeClient", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(monitor, commands);
        var snapshot = typeof(DeviceMonitor).GetField("snapshot", BindingFlags.Instance | BindingFlags.NonPublic)!;
        foreach (var status in new[]
        {
            Ready() with { Connected = false }, Ready() with { Hardware = Hardware(8) },
            Ready() with { Msc = null }, Ready() with { Msc = new(true, false, false) },
            Ready() with { Msc = new(false, true, false) }, Ready() with { RebootPending = true },
            Ready() with { StorageStale = true },
            Ready() with { CommandOutcomeUncertain = true },
            Ready() with { Hardware = Hardware(0xa8) },
            Ready() with { SelfTest = new(0, "writing", 0, 0, 0, 0, 0, 0, 0, 0, 0, false, true) },
            Ready() with { Hardware = Hardware(0xa8),
                SelfTest = new(0, "idle", 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false),
                SelfTestStale = true }
        })
        {
            snapshot.SetValue(monitor, status);
            Assert.Throws<InvalidOperationException>(() => monitor.SetWireless(Settings()));
            Assert.Equal(0, commands.Writes);
        }
        snapshot.SetValue(monitor, Ready());
        var saved = monitor.SetWireless(Settings());
        Assert.True(saved.RebootPending);
        Assert.Equal(Board, saved.Settings);
        Assert.True(monitor.Snapshot.RebootPending);
        Assert.Equal(Board, monitor.Snapshot.Settings);
        Assert.Equal(1, commands.Writes);
    }

    [Fact]
    public async Task HttpValidatesRoutesAndNeverReturnsCredentials()
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
        foreach (var body in new[] { "null", "[]", "{}", "{\"password\":\"secret\\n12\"}",
                     "{\"wifi_enabled\":true,\"ble_enabled\":false,\"ssid\":\"Pico\",\"confirm_reboot\":1}" })
        {
            var response = await client.PostAsync("/api/wireless?board=a",
                new StringContent(body, Encoding.UTF8, "application/json"));
            Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
            Assert.DoesNotContain("secret", await response.Content.ReadAsStringAsync());
        }
        Assert.Equal(0, dashboard.Writes);
        var saved = await client.PostAsJsonAsync("/api/wireless?board=a",
            new { wifi_enabled = true, ble_enabled = true, ssid = "Pico網",
                password = "secret12", ble_key = new string('a', 64), confirm_reboot = true,
                board = new { system_clock_mhz = 200, sdcard_profile = 1, radios = 3 } });
        Assert.Equal(HttpStatusCode.OK, saved.StatusCode);
        var text = await saved.Content.ReadAsStringAsync();
        Assert.DoesNotContain("secret12", text);
        Assert.DoesNotContain(new string('a', 64), text);
        Assert.Equal("a", dashboard.Board);
        using var parsed = JsonDocument.Parse(text);
        Assert.True(parsed.RootElement.GetProperty("reboot_pending").GetBoolean());
        Assert.Equal("connected", parsed.RootElement.GetProperty("wireless").GetProperty("wifi_state").GetString());
        var scanned = await client.PostAsync("/api/wireless/scan?board=a", null);
        Assert.Equal(HttpStatusCode.OK, scanned.StatusCode);
        Assert.Equal("a", dashboard.Board);
        var tested = await client.PostAsJsonAsync("/api/wireless/test?board=a",
            new { ssid = "Pico網", password = "secret12" });
        Assert.Equal(HttpStatusCode.OK, tested.StatusCode);
        var testText = await tested.Content.ReadAsStringAsync();
        Assert.DoesNotContain("secret12", testText);
        Assert.Equal(HttpStatusCode.OK, (await client.PostAsync("/api/wireless/cancel?board=a", null)).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync("/api/wireless?board=b")).StatusCode);
        Assert.Equal("b", dashboard.Board);
        dashboard.Failure = new IOException("secret12");
        var failed = await client.PostAsJsonAsync("/api/wireless?board=a",
            new { wifi_enabled = false, ble_enabled = false, ssid = "", confirm_reboot = true,
                board = new { system_clock_mhz = 0, sdcard_profile = 0, radios = 0 } });
        Assert.Equal(HttpStatusCode.ServiceUnavailable, failed.StatusCode);
        Assert.DoesNotContain("secret12", await failed.Content.ReadAsStringAsync());
        await app.StopAsync();
    }

    private sealed class Commands : IHidCommands
    {
        public int Writes { get; private set; }
        public bool CommandOutcomeUncertain => false;
        public void SetLed(bool on) { }
        public void SetMsc(bool enabled, bool force = false) { }
        public BoardConfigurationResponse ReadConfiguration() => throw new NotSupportedException();
        public BootSettingsResponse ReadSettings() => new(0, 0);
        public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false) => settings;
        public void Reboot() { }
        public void SetSelfTest(string action, uint sizeMib) { }
        public WirelessStatusResponse SetWireless(WirelessRequest settings)
        {
            Writes++;
            return HidProtocol.ParseWirelessStatus(Report());
        }
    }

    private sealed class Dashboard : IDashboardBoards
    {
        public string? Board { get; private set; }
        public int Writes { get; private set; }
        public Exception? Failure { get; set; }
        public DashboardBoardList Boards() => new([], null);
        public StatusResponse Snapshot(string? boardId) => Ready();
        public bool SetLed(string? boardId, bool on) => true;
        public MscStatusResponse? SetMsc(string? boardId, bool enabled, bool force = false) => null;
        public BootSettingsResponse? SetSettings(string? boardId, BootSettingsResponse settings, bool force = false) => settings;
        public bool Reboot(string? boardId) => true;
        public bool SetSelfTest(string? boardId, string action, uint sizeMib) => true;
        public WirelessResponse ReadWireless(string? boardId)
        {
            Board = boardId;
            return new(HidProtocol.ParseWirelessStatus(Report()), false);
        }
        public WirelessResponse SetWireless(string? boardId, WirelessRequest settings)
        {
            if (Failure is not null) throw Failure;
            Writes++;
            Board = boardId;
            return new(HidProtocol.ParseWirelessStatus(Report()), true);
        }
        public IReadOnlyList<WirelessNetworkResponse> ScanWireless(string? boardId)
        {
            Board = boardId;
            return [new("Pico網", -42, true)];
        }
        public WirelessOperationResponse TestWireless(string? boardId, string ssid, string password)
        {
            Board = boardId;
            Assert.Equal("Pico網", ssid);
            Assert.Equal("secret12", password);
            return new(1, 5, 0, 0, 2, "192.168.1.2", []);
        }
        public bool CancelWirelessTest(string? boardId) { Board = boardId; return true; }
    }
}
