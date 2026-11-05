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
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    public void WirelessStatusAcceptsHidBackendPrefixes(byte version)
    {
        var report = new byte[version >= 4 ? 54 : 53];
        Report().CopyTo(report, 0);
        report[1] = version;
        var expected = HidProtocol.ParseWirelessStatus(report);
        Assert.Equal(version >= 2, expected.TcpCliConfigurable);
        foreach (var prefix in new byte[] { 0, HidProtocol.WirelessReportId })
        {
            var framed = new byte[256];
            framed[0] = prefix;
            report.CopyTo(framed, 1);
            Assert.Equal(expected, HidProtocol.ParseWirelessStatus(framed));
            framed[report.Length + 1] = 1;
            Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(framed));
        }
    }

    [Fact]
    public void ParsesFirmwareGeneratedFeature30StatusVectors()
    {
        using var vectors = JsonDocument.Parse(File.ReadAllText(
            Path.Combine(AppContext.BaseDirectory, "wireless-status-v4.json")));
        WirelessStatusResponse Parse(string name)
        {
            var report = new byte[54];
            report[0] = HidProtocol.WirelessReportId;
            Convert.FromHexString(vectors.RootElement.GetProperty(name).GetString()!).CopyTo(report, 1);
            return HidProtocol.ParseWirelessStatus(report);
        }

        foreach (var (name, cliEnabled, authRequired, serving) in new[]
                 {
                     ("tcp_cli_auth_required", true, true, true),
                     ("tcp_cli_disabled", false, false, false),
                 })
        {
            var status = Parse(name);
            Assert.Equal(cliEnabled, status.TcpCliEnabled);
            Assert.Equal(authRequired, status.TcpCliAuthRequired);
            Assert.Equal(serving, status.TcpCliStarted);
            Assert.Equal("PicoForge", status.Ssid);
            Assert.Equal("connected", status.WifiState);
            Assert.True(status.RestartRequired);
            Assert.Null(status.Rssi);
        }

        var diagnostics = Parse("diagnostics");
        Assert.Equal(2, diagnostics.Error);
        Assert.Equal("sae", diagnostics.WifiAuthState);
        Assert.Equal(0x0102, diagnostics.ReconnectCount);
        Assert.Equal(-61, diagnostics.Rssi);
    }

    [Fact]
    public void V2TcpSwitchSecurityAndExpiryDetails()
    {
        var request = Settings();
        Assert.Equal(19, HidProtocol.BuildWirelessRequest(request, 1)[4]);
        Assert.Equal(3, HidProtocol.BuildWirelessRequest(request, 1, false)[4]);
        request.TcpCliEnabled = false;
        Assert.Equal(11, HidProtocol.BuildWirelessRequest(request, 1)[4]);
        Assert.Throws<ArgumentException>(() => HidProtocol.BuildWirelessRequest(request, 1, false));
        request.TcpCliEnabled = true;
        Assert.Equal(3, HidProtocol.BuildWirelessRequest(request, 1)[4]);
        var status = Report();
        status[1] = 3;
        status[48] = 1;
        Assert.True(HidProtocol.ParseWirelessStatus(status).TcpCliConfigurable);
        Assert.False(HidProtocol.ParseWirelessStatus(status).TcpCliEnabled);
        status[48] = 2;
        Assert.True(HidProtocol.ParseWirelessStatus(status).TcpCliAuthRequired);
        status[48] = 4;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(status));
        var statusV4 = new byte[54];
        Report().CopyTo(statusV4, 0);
        statusV4[1] = 4;
        statusV4[50] = 7;
        statusV4[53] = 1;
        var parsedV4 = HidProtocol.ParseWirelessStatus(statusV4);
        Assert.Equal((ushort)7, parsedV4.ReconnectCount);
        Assert.True(parsedV4.RestartRequired);
        string[] labels = ["Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)", "RSN (WPA2/WPA3)",
            "WPA/RSN", "WPA/RSN"];
        var report = OperationResult();
        report[1] = 2;
        report[5] = 1;
        report[7] = 1;
        report[8] = (byte)'x';
        for (byte security = 0; security < 8; security++)
        {
            report[41] = security;
            var network = HidProtocol.ParseWirelessOperation(report).Networks[0];
            Assert.Equal(labels[security], network.Security);
            Assert.Equal(security != 0, network.Secured);
        }
        report[41] = 8;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessOperation(report));
        foreach (var (detail, phase) in new[] { (9, "Test connection lost"), (10, "Test credentials expired") })
        {
            report = OperationResult(state: 6, detail: (byte)detail);
            report[1] = 2;
            Assert.Equal(phase, HidProtocol.ParseWirelessOperation(report).Phase);
        }
        report[4] = 11;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessOperation(report));
        report = OperationResult(state: 5);
        report[221] = 88;
        report[222] = 2;
        Assert.Equal((ushort)600, HidProtocol.ParseWirelessOperation(report).RemainingSeconds);
        report[221] = 89;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessOperation(report));
    }

    [Fact]
    public void ParsedWirelessResponsesDoNotRetainReusableFeatureBuffer()
    {
        var statusReport = Report();
        var status = HidProtocol.ParseWirelessStatus(statusReport);
        Array.Clear(statusReport);
        Assert.Equal("Pico網", status.Ssid);
        Assert.Equal("connected", status.WifiState);

        var operationReport = OperationResult(state: 2);
        operationReport[1] = 1;
        operationReport[5] = 1;
        operationReport[7] = 1;
        operationReport[8] = (byte)'x';
        operationReport[41] = 1;
        var operation = HidProtocol.ParseWirelessOperation(operationReport);
        Array.Clear(operationReport);
        var network = Assert.Single(operation.Networks);
        Assert.Equal("x", network.Ssid);
        Assert.Equal("Secured", network.Security);
    }

    private static WirelessRequest Settings() => new()
    {
        WifiEnabled = true, BleEnabled = true, Ssid = "Pico網", ConfirmReboot = true, Board = Board
    };

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void TcpSwitchSaveVerifiesReadback(bool mismatch)
    {
        var before = Report(0, 0);
        var after = Report();
        before[1] = after[1] = 2;
        after[48] = mismatch ? (byte)0 : (byte)1;
        var reads = new Queue<byte[]>([before, after]);
        var writes = new List<byte[]>();
        using var hid = new PicoForgeHid(Hardware(), (id, _) => id == 2 ? Configuration() : reads.Dequeue(),
            writes.Add, _ => Assert.Fail("No reconnect"));
        var request = Settings();
        request.TcpCliEnabled = false;
        if (mismatch) Assert.Throws<InvalidOperationException>(() => hid.SetWireless(request));
        else Assert.False(hid.SetWireless(request).TcpCliEnabled);
        Assert.Equal(11, Assert.Single(writes)[4]);
        Assert.Equal(mismatch, hid.CommandOutcomeUncertain);
    }

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
        var report = new byte[53];
        new byte[] { 30, 3, 3, status, transaction, 2, 192, 168, 1, 2 }.CopyTo(report, 0);
        Encoding.UTF8.GetBytes("Pico網").CopyTo(report, 10);
        report[43] = report[44] = 1;
        return report;
    }

    private static byte[] OperationResult(byte state = 2, byte transaction = 1, byte detail = 0)
    {
        var report = new byte[223];
        report[0] = 35;
        report[1] = 2;
        report[2] = transaction;
        report[3] = state;
        report[4] = detail;
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
        Assert.Equal(new byte[] { 31, 2, 255, 3, 19 }, report[..5]);
        Assert.Equal(Encoding.UTF8.GetBytes("Pico網"), report[5..12]);
        Assert.All(report[38..134], value => Assert.Equal(0, value));
        Assert.Equal(new byte[] { 200, 0, 1 }, report[134..]);
        var request = Settings();
        request.Password = "secret12";
        request.BleKey = new string('a', 64);
        report = HidProtocol.BuildWirelessRequest(request, 1);
        Assert.Equal(16, report[4]);
        Assert.Equal(Encoding.ASCII.GetBytes("secret12\0"), report[38..47]);
        Assert.All(report[102..134], value => Assert.Equal(0xaa, value));
        Assert.Equal(new byte[] { 200, 0, 1 }, report[134..]);
        request.WifiEnabled = false;
        Assert.Throws<ArgumentException>(() => HidProtocol.BuildWirelessRequest(request, 2));
        var erase = Settings();
        erase.WifiEnabled = false;
        erase.BleEnabled = false;
        erase.Ssid = "";
        erase.Password = "";
        erase.BleKey = "";
        erase.Board = new(200, 1, 0);
        report = HidProtocol.BuildWirelessRequest(erase, 3);
        Assert.Equal(16, report[4]);
        Assert.All(report[5..134], value => Assert.Equal(0, value));
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
        Assert.Equal(new WirelessNetworkResponse("test", -42, true, "WEP"), decoded.Networks[0]);
        Assert.DoesNotContain("secret12", JsonSerializer.Serialize(decoded));
        result[41] = 8;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessOperation(result));
        Assert.Equal("Could not start connection",
            HidProtocol.ParseWirelessOperation(OperationResult(state: 6, detail: 8)).Phase);
        Assert.Throws<InvalidOperationException>(
            () => HidProtocol.ParseWirelessOperation(OperationResult(state: 6, detail: 11)));
    }

    [Theory]
    [InlineData("\"wifi_enabled\":1")]
    [InlineData("\"ble_enabled\":null")]
    [InlineData("\"tcp_cli_enabled\":null")]
    [InlineData("\"tcp_cli_enabled\":1")]
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
    [InlineData("\"tcp_cli_auth_required\":true")]
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
        Assert.Equal("off", result.BleState);
        Assert.False(result.TcpCliStarted);
        var connected = Report();
        connected[46] = 2;
        Assert.Equal("connected", HidProtocol.ParseWirelessStatus(connected).BleState);
        connected[47] = 1;
        Assert.True(HidProtocol.ParseWirelessStatus(connected).TcpCliStarted);
        var json = JsonSerializer.Serialize(result);
        Assert.DoesNotContain("\"Password\":", json);
        Assert.DoesNotContain("\"BleKey\":", json);
        foreach (var (offset, value) in new (int, byte)[]
                 { (1, 4), (2, 4), (3, 7), (5, 4), (43, 2), (44, 2), (45, 4), (46, 3),
                   (47, 4), (48, 4), (49, 4) })
        {
            var report = Report();
            report[offset] = value;
            Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(report));
        }
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessStatus(Report()[..52]));
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
            Assert.Equal(54, size);
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
            id == 30 ? Report(0, 0) : OperationResult(6, 1, 5),
            _ => { }, _ => Assert.Fail("No reconnect"));
        Assert.Contains("Could not connect: Password rejected",
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

    [Fact]
    public void WirelessOperationPumpsInputsWithoutReplayingTheTest()
    {
        var reads = 0;
        var polls = 0;
        var writes = 0;
        using var hid = new PicoForgeHid(Hardware(), (id, _) =>
            id == 30 ? Report(0, 0) : OperationResult(++reads == 1 ? (byte)4 : (byte)5, 1, 2),
            _ => writes++, _ => Assert.Fail("No reconnect"));
        var phases = new List<string>();
        Assert.Equal(5, hid.TestWireless("Pico網", "secret12", () => polls++, phases.Add).State);
        Assert.Equal(["Checking password"], phases);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseWirelessOperation(OperationResult(6, 1, 255)));
        Assert.Equal(2, polls);
        Assert.Equal(1, writes);
        Assert.False(hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void MonitorPublishesLiveReportsWhileTheWirelessTestOwnsTheDeviceGate()
    {
        using var monitor = new DeviceMonitor();
        var commands = new Commands();
        var runtime = new RuntimeStatusResponse(1, 1000, 150000000, true, false, true, 2500, false, false);
        var memory = new MemoryStatusResponse(1, true, true, false, false, "sd-card", "sdio",
            null, null, null, null);
        var msc = new MscStatusResponse(false, false, false);
        var selfTest = new SelfTestStatusResponse(1, "idle", 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false);
        commands.Inputs.Enqueue(new(runtime, null, null));
        commands.Inputs.Enqueue(new(null, memory, msc, FilesystemFormat: "unknown"));
        commands.Inputs.Enqueue(new(null, null, null, selfTest));
        commands.AfterPoll = () =>
        {
            Assert.Equal("Waiting for IP address", monitor.Snapshot.WirelessPhase);
            Assert.Equal(runtime, monitor.Snapshot.Runtime);
            Assert.Equal(memory, monitor.Snapshot.Memory);
            Assert.Equal(msc, monitor.Snapshot.Msc);
            Assert.Equal(selfTest, monitor.Snapshot.SelfTest);
            Assert.False(monitor.Snapshot.TelemetryStale);
            Assert.False(monitor.Snapshot.StorageStale);
            Assert.False(monitor.Snapshot.SelfTestStale);
        };
        typeof(DeviceMonitor).GetField("activeClient", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(monitor, commands);
        var snapshot = typeof(DeviceMonitor).GetField("snapshot", BindingFlags.Instance | BindingFlags.NonPublic)!;
        snapshot.SetValue(monitor, Ready());
        Assert.Equal(5, monitor.TestWireless("Pico網", "secret12").State);
        Assert.Null(monitor.Snapshot.WirelessPhase);
        Assert.Equal(4, commands.InputReads);
        Assert.Single(commands.InputTimeouts.Distinct());
        Assert.Equal(0, commands.InputTimeouts[0]);
        snapshot.SetValue(monitor, Ready() with { StorageStale = true });
        Assert.Throws<InvalidOperationException>(() => monitor.TestWireless("Pico網", "secret12"));
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
        public Queue<InputReport> Inputs { get; } = new();
        public List<int> InputTimeouts { get; } = new();
        public int InputReads => InputTimeouts.Count;
        public Action? AfterPoll { get; set; }
        public bool CommandOutcomeUncertain => false;
        public InputReport? ReadInputReport(int timeoutMilliseconds = 250)
        {
            InputTimeouts.Add(timeoutMilliseconds);
            return Inputs.TryDequeue(out var report) ? report : null;
        }
        public WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
            Action<string>? onProgress = null)
        {
            onProgress?.Invoke("Waiting for IP address");
            onPoll?.Invoke();
            AfterPoll?.Invoke();
            return HidProtocol.ParseWirelessOperation(OperationResult(5));
        }
        public void SetLed(bool on) { }
        public void SetMsc(bool enabled, bool force = false) { }
        public BoardConfigurationResponse ReadConfiguration() => throw new NotSupportedException();
        public BootSettingsResponse ReadSettings() => new(0, 0);
        public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false) => settings;
        public void Reboot() { }
        public void SetSelfTest(string action, uint sizeMib) { }
        public WirelessStatusResponse ReadWireless() => throw new NotSupportedException();
        public IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null) =>
            throw new NotSupportedException();
        public void CancelWirelessTest() => throw new NotSupportedException();
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
