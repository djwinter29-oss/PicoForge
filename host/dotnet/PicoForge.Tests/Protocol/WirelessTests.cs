using System.Buffers.Binary;
using System.Net;
using System.Net.Http.Json;
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
    private static readonly HardwareInfoResponse Hardware =
        new("PicoForge", "RP2350", 2, "0001020304050607", Capabilities: 0x8f);

    private static byte[] Configuration(byte flags = 0x38) =>
        [2, 1, 200, 0, 1, 255, 255, 255, 2, 3, 4, 5, 255, 255, flags, 0, 255, 255];

    private static byte[] Status()
    {
        var report = new byte[49];
        report[0] = 12;
        report[1] = 1;
        report[5] = 3;
        report[10] = 3;
        report[11] = (byte)'A';
        report[12] = 0;
        report[13] = 255;
        return report;
    }

    private static WirelessRequest Settings() => new()
    {
        WifiEnabled = true, BleEnabled = true, Board = new(200, 1, 3), ConfirmReboot = true
    };

    private static byte[] Result(byte opcode, byte status = 2, byte error = 0, byte detail = 0)
    {
        var report = new byte[69];
        report[0] = 20;
        report[1] = report[2] = 1;
        report[3] = opcode;
        report[4] = status;
        report[5] = error;
        report[6] = detail;
        return report;
    }

    [Fact]
    public void RuntimeStatusUsesReport12AndSavedSwitchesUseReport2()
    {
        var report = Status();
        var configuration = HidProtocol.ParseConfigurationReport(Configuration(), Hardware);
        var status = HidProtocol.ParseWirelessStatus(report, configuration);
        Assert.Equal("A\\x00\\xFF", status.Ssid);
        Assert.Equal("4100FF", status.SsidHex);
        Assert.True(status.WifiEnabled);
        Assert.True(status.BleEnabled);
        Assert.True(status.TcpCliEnabled);
        Assert.True(status.NetworkConfigured);
        Assert.True(status.BleKeyConfigured);
        report[12] = 1;
        Assert.Equal("4100FF", status.SsidHex);
        var json = JsonSerializer.Serialize(status, new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower
        });
        foreach (var removed in new[] { "password_configured", "wifi_auth_state", "rssi", "reconnect_count",
            "restart_required", "transaction_id" })
            Assert.DoesNotContain($"\"{removed}\"", json);
    }

    [Fact]
    public void SharedFirmwareStatusVectorsUseTheFrozenReport12Contract()
    {
        using var fixtures = JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory,
            "wireless-status.json")));
        var configuration = HidProtocol.ParseConfigurationReport(Configuration(), Hardware);
        foreach (var name in new[] { "tcp_cli_auth_required", "tcp_cli_disabled", "tested_candidate",
            "test_failed_saved_link_serving", "radio_init_failed_while_connected" })
        {
            var report = new byte[] { 12 }.Concat(Convert.FromHexString(
                fixtures.RootElement.GetProperty(name).GetString()!)).ToArray();
            var status = HidProtocol.ParseWirelessStatus(report, configuration);
            Assert.Equal("PicoForge", status.Ssid);
            Assert.Equal("5069636F466F726765", status.SsidHex);
            Assert.Equal(name is "tested_candidate" or "test_failed_saved_link_serving", status.PairTested);
            Assert.Equal(name is "tcp_cli_auth_required" or "test_failed_saved_link_serving", status.TcpCliStarted);
        }
    }

    [Theory]
    [InlineData(0, 30)]
    [InlineData(1, 2)]
    [InlineData(2, 7)]
    [InlineData(4, 3)]
    [InlineData(10, 33)]
    [InlineData(43, 4)]
    [InlineData(46, 1)]
    [InlineData(48, 16)]
    public void RejectsMalformedStatus(int index, byte value)
    {
        var report = Status();
        report[index] = value;
        Assert.Throws<InvalidDataException>(() => HidProtocol.ParseWirelessStatus(report,
            HidProtocol.ParseConfigurationReport(Configuration(), Hardware)));
    }

    [Fact]
    public void ReservedStatusDetailAndFlagAreIgnoredButExpiryIsValidated()
    {
        var report = Status();
        report[3] = 255;
        report[5] |= 8;
        var configuration = HidProtocol.ParseConfigurationReport(Configuration(), Hardware);
        Assert.Equal(0, HidProtocol.ParseWirelessStatus(report, configuration).Error);
        report[48] = 8;
        Assert.Equal(8, HidProtocol.ParseWirelessStatus(report, configuration).StagedState);
        report[5] |= 16;
        Assert.Throws<InvalidDataException>(() => HidProtocol.ParseWirelessStatus(report, configuration));
    }

    [Fact]
    public void FeatureReportsRequireExactHidSharpFraming()
    {
        var configuration = HidProtocol.ParseConfigurationReport(Configuration(), Hardware);
        var report = Status();
        foreach (var malformed in new[] { report[1..], new byte[] { 0 }.Concat(report).ToArray(),
            new byte[] { 12 }.Concat(report).ToArray(), report.Concat(new byte[] { 0 }).ToArray() })
            Assert.Throws<InvalidDataException>(() => HidProtocol.ParseWirelessStatus(malformed, configuration));
        Assert.Throws<InvalidDataException>(() => HidProtocol.ParseControlResult(new byte[10]));
    }

    [Theory]
    [InlineData("é", "C3A9")]
    [InlineData("ignored escaped label", "4100ff")]
    [InlineData("", "")]
    public void OpaqueSsidBytesRoundTrip(string display, string hex)
    {
        Assert.Equal(Convert.FromHexString(hex), HidProtocol.EncodeSsid(display, hex));
    }

    [Theory]
    [InlineData("a")]
    [InlineData("GG")]
    public void RejectsInvalidHex(string hex) =>
        Assert.Throws<ArgumentException>(() => HidProtocol.EncodeSsid("ignored", hex));

    [Fact]
    public void PrintableUtf8IsDisplayedAndInvalidOrControlBytesAreEscaped()
    {
        Assert.Equal("Pico網", HidProtocol.DisplaySsid(Encoding.UTF8.GetBytes("Pico網")));
        Assert.Equal("\\xC3(\\x0A", HidProtocol.DisplaySsid([0xc3, 0x28, 10]));
        Assert.Equal("\\xE2\\x80\\xAE", HidProtocol.DisplaySsid([0xe2, 0x80, 0xae]));
    }

    [Theory]
    [InlineData(1, 5, 5, 0)]
    [InlineData(2, 3, 3, 0)]
    [InlineData(5, 3, 2, 0)]
    [InlineData(6, 1, 0, 0)]
    [InlineData(10, 5, 5, 0)]
    [InlineData(13, 1, 0, 0)]
    [InlineData(13, 5, 5, 0)]
    [InlineData(6, 3, 3, 17)]
    [InlineData(12, 3, 3, 24)]
    [InlineData(5, 5, 5, 28)]
    [InlineData(4, 6, 6, 8)]
    [InlineData(4, 3, 3, 0)]
    [InlineData(1, 4, 4, 0)]
    [InlineData(9, 4, 7, 0)]
    public void RejectsDisallowedControlCombinations(byte opcode, byte status, byte error, byte detail) =>
        Assert.Throws<InvalidDataException>(() =>
            HidProtocol.ParseControlResult(Result(opcode, status, error, detail)));

    [Theory]
    [InlineData(6, 3, 3, 16)]
    [InlineData(7, 3, 3, 17)]
    [InlineData(8, 3, 3, 18)]
    [InlineData(5, 3, 3, 27)]
    [InlineData(9, 3, 3, 20)]
    [InlineData(12, 3, 3, 22)]
    [InlineData(13, 3, 3, 24)]
    [InlineData(5, 5, 5, 25)]
    [InlineData(12, 5, 5, 26)]
    [InlineData(4, 6, 6, 10)]
    [InlineData(255, 4, 7, 0)]
    public void AcceptsDefinedControlDetails(byte opcode, byte status, byte error, byte detail)
    {
        var report = Result(opcode, status, error, detail);
        Assert.Equal(detail, HidProtocol.ParseControlResult(report).ResultData[0]);
        report[7] = 1;
        Assert.Throws<InvalidDataException>(() => HidProtocol.ParseControlResult(report));
    }

    [Fact]
    public void ProvisionStagesSeparatelyAndSavesOnceWithExactMasks()
    {
        using var wire = new Wire();
        var settings = Settings();
        settings.SsidHex = "4100FF";
        settings.Password = new string('p', 63);
        settings.BleKey = new string('a', 64);
        wire.Hid.SetWireless(settings);
        Assert.Equal(new byte[] { 6, 7, 8, 5 }, wire.Writes.Select(report => report[3]));
        Assert.All(wire.Writes, report => Assert.Equal(69, report.Length));
        Assert.Equal(new byte[] { 20, 1, 1, 6, 0, 3, 65, 0, 255 }, wire.Writes[0][..9]);
        Assert.All(wire.Writes[0][9..], value => Assert.Equal(0, value));
        Assert.Equal(63, wire.Writes[1][5]);
        Assert.All(wire.Writes[1][6..], value => Assert.Equal((byte)'p', value));
        Assert.Equal(32, wire.Writes[2][5]);
        Assert.All(wire.Writes[2][6..38], value => Assert.Equal(0xaa, value));
        Assert.All(wire.Writes[2][38..], value => Assert.Equal(0, value));
        Assert.Equal(new byte[] { 20, 1, 4, 5, 2, 200, 0, 1, 7 }, wire.Writes[3][..9]);
        Assert.False(wire.Hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void OmittedCredentialsAreNotStagedAndDisabledTcpSwitchIsPreserved()
    {
        using var wire = new Wire { ConfigurationFlags = 0x18 };
        wire.Hid.SetWireless(Settings());
        var save = Assert.Single(wire.Writes);
        Assert.Equal(5, save[3]);
        Assert.Equal(0, save[4]);
        Assert.Equal(3, save[8]);
    }

    [Fact]
    public void SaveResponseContainsConfirmedSwitchesAndOpaqueSsid()
    {
        using var wire = new Wire { ConfigurationFlags = 0 };
        var settings = Settings();
        settings.TcpCliEnabled = true;
        settings.SsidHex = "4100FF";
        settings.Password = "";
        var saved = wire.Hid.SetWireless(settings);
        Assert.True(saved.WifiEnabled);
        Assert.True(saved.BleEnabled);
        Assert.True(saved.TcpCliEnabled);
        Assert.Equal("4100FF", saved.SsidHex);
    }

    [Theory]
    [InlineData(64, true)]
    [InlineData(0, false)]
    public void OnlyEmptyControlKeyClearsSavedKey(int hexLength, bool configured)
    {
        using var wire = new Wire();
        var settings = Settings();
        settings.BleKey = new string('0', hexLength);
        var saved = wire.Hid.SetWireless(settings);
        Assert.Equal(configured, saved.BleKeyConfigured);
        Assert.Equal(hexLength / 2, wire.Writes[0][5]);
        Assert.All(wire.Writes[0][6..], value => Assert.Equal(0, value));
    }

    [Fact]
    public void SettingsOnlySaveReadsReport2AndPreservesTcpSwitch()
    {
        using var wire = new Wire();
        Assert.Equal(new BootSettingsResponse(150, 0, 0), wire.Hid.SetSettings(new(150, 0, 0)));
        var save = Assert.Single(wire.Writes);
        Assert.Equal(5, save[3]);
        Assert.Equal(4, save[8]);
        Assert.Equal(0, save[4]);
    }

    [Fact]
    public void MissingConfigurationDoesNotDefaultTcpSwitchOrWrite()
    {
        using var wire = new Wire { FailConfiguration = true };
        Assert.Throws<IOException>(() => wire.Hid.SetSettings(new(200, 1, 0)));
        Assert.Empty(wire.Writes);
    }

    [Fact]
    public void TestedSaveReusesExactStagedPairWithoutRestaging()
    {
        using var wire = new Wire();
        var test = wire.Hid.TestWireless("escaped label", "secret12", ssidHex: "4100ff");
        Assert.Equal(1, test.TestGeneration);
        Assert.Equal(600, test.RemainingSeconds);
        var settings = Settings();
        settings.UseTestedWifi = true;
        settings.SsidHex = "4100ff";
        wire.Hid.SetWireless(settings);
        Assert.Equal(new byte[] { 6, 7, 9, 5 }, wire.Writes.Select(report => report[3]));
        Assert.Equal(2, wire.Writes[^1][4]);
    }

    [Fact]
    public void TestedSaveRejectsDifferentPairBeforeAnyWrite()
    {
        using var wire = new Wire();
        var settings = Settings();
        settings.UseTestedWifi = true;
        settings.Ssid = "other";
        Assert.Throws<BoardStateException>(() => wire.Hid.SetWireless(settings));
        Assert.Empty(wire.Writes);
    }

    [Fact]
    public void OmittedCredentialsCannotSilentlyCommitPreviousStaging()
    {
        using var wire = new Wire();
        wire.StatusReport[5] |= 0x30;
        Assert.Throws<BoardStateException>(() => wire.Hid.SetWireless(Settings()));
        Assert.Empty(wire.Writes);
    }

    [Fact]
    public void ScanUsesGenerationAndIndividualEntryCommand()
    {
        using var wire = new Wire();
        var network = Assert.Single(wire.Hid.ScanWireless());
        Assert.Equal("A\\x00\\xFF", network.Ssid);
        Assert.Equal("4100FF", network.SsidHex);
        Assert.Equal(-42, network.Rssi);
        Assert.Equal(new byte[] { 11, 12 }, wire.Writes.Select(report => report[3]));
        Assert.Equal(new byte[] { 0, 1, 0 }, wire.Writes[1][5..8]);
    }

    [Fact]
    public void StaleScanEntryCannotBeShownAsCurrent()
    {
        using var wire = new Wire { StaleScanEntry = true };
        Assert.Throws<InvalidDataException>(() => wire.Hid.ScanWireless());
        // Opcode 12 only reads a result, so a mismatched entry is bad data, not an unresolved write.
        Assert.False(wire.Hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void TestGenerationWrapAndInputPumpArePreserved()
    {
        using var wire = new Wire();
        wire.StatusReport[47] = 255;
        var polls = 0;
        var result = wire.Hid.TestWireless("Pico", "secret12", () => polls++);
        Assert.Equal(1, result.TestGeneration);
        Assert.True(polls > 0);
    }

    [Fact]
    public void CancelledTestSendsCancelAndLeavesOutcomeKnown()
    {
        using var wire = new Wire();
        using var stop = new CancellationTokenSource();
        stop.Cancel();
        Assert.Throws<WirelessTestCancelledException>(() =>
            wire.Hid.TestWireless("Pico", "secret12", cancel: stop.Token));
        Assert.Equal(new byte[] { 6, 7, 9, 10 }, wire.Writes.Select(report => report[3]));
        Assert.False(wire.Hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void CancelUsesOpcode10AndDiscardsStagingWithoutCancellingScan()
    {
        using var wire = new Wire();
        wire.StatusReport[5] |= 16;
        wire.StatusReport[43] = 1;
        wire.StatusReport[44] = 1;
        wire.Hid.CancelWirelessTest();
        Assert.Equal(10, Assert.Single(wire.Writes)[3]);
        Assert.Equal(1, wire.StatusReport[43]);
    }

    [Fact]
    public void ProofUsesCommonEnvelopeAndRetriesOnlyGet()
    {
        using var wire = new Wire { ProofReadFailure = true };
        var nonce = Enumerable.Range(0, 16).Select(value => (byte)value).ToArray();
        Assert.Equal(Enumerable.Repeat((byte)0xab, 16), wire.Hid.TcpCliAuthProof(nonce,
            Convert.FromHexString(Hardware.SerialNumber)));
        var request = Assert.Single(wire.Writes);
        Assert.Equal(13, request[3]);
        Assert.Equal(nonce, request[5..21]);
        Assert.All(request[21..], value => Assert.Equal(0, value));
        Assert.Equal(1, wire.Reopens);
    }

    [Fact]
    public void ProofRejectsDifferentUidWithoutTouchingResultSlot()
    {
        using var wire = new Wire();
        Assert.Throws<InvalidDataException>(() => wire.Hid.TcpCliAuthProof(new byte[16], new byte[8]));
        Assert.Empty(wire.Writes);
    }

    [Fact]
    public void UncertainSaveNeverReplaysAndBlocksEveryLaterCommandIncludingProof()
    {
        using var wire = new Wire { SaveDisconnect = true };
        Assert.Throws<IOException>(() => wire.Hid.SetWireless(Settings()));
        Assert.True(wire.Hid.CommandOutcomeUncertain);
        Assert.Throws<BoardStateException>(() => wire.Hid.CancelWirelessTest());
        Assert.Throws<BoardStateException>(() => wire.Hid.TcpCliAuthProof(new byte[16],
            Convert.FromHexString(Hardware.SerialNumber)));
        Assert.Single(wire.Writes);
    }

    [Fact]
    public void SaveMismatchRemainsUncertain()
    {
        using var wire = new Wire { WrongSaveMask = true };
        Assert.Throws<InvalidDataException>(() => wire.Hid.SetWireless(Settings()));
        Assert.True(wire.Hid.CommandOutcomeUncertain);
        Assert.Single(wire.Writes);
    }

    [Fact]
    public void ReconcileClearsUncertaintyOnlyForTheExactFinalTransaction()
    {
        using var wire = new Wire { SaveDisconnect = true };
        Assert.Throws<IOException>(() => wire.Hid.SetWireless(Settings()));
        var pending = wire.Hid.UncertainCommand;
        Assert.NotNull(pending);
        Assert.True(wire.Hid.CommandOutcomeUncertain);

        wire.SaveDisconnect = false;
        Assert.Null(wire.Hid.ReconcileOutcome(new(pending!.Value.TransactionId, Opcode.Led), null));
        Assert.True(wire.Hid.CommandOutcomeUncertain);

        var result = wire.Hid.ReconcileOutcome(pending, null);
        Assert.NotNull(result?.ControlResult);
        Assert.False(wire.Hid.CommandOutcomeUncertain);
    }

    [Theory]
    [InlineData("scan")]
    [InlineData("test")]
    public void ReconcileRequiresTheExactTerminalWirelessGeneration(string kind)
    {
        var clock = new ManualTime();
        using var wire = new Wire(clock) { NeverFinishScan = kind == "scan", NeverFinishTest = kind == "test" };
        Assert.Throws<TimeoutException>(() =>
        {
            if (kind == "scan")
                wire.Hid.ScanWireless(() => clock.Advance(TimeSpan.FromSeconds(16)));
            else
                wire.Hid.TestWireless("PicoForge", "password",
                    () => clock.Advance(TimeSpan.FromSeconds(36)));
        });
        var pending = wire.Hid.UncertainWirelessOperation;
        Assert.NotNull(pending);
        Assert.Equal(kind, pending.Value.Kind);
        Assert.True(wire.Hid.CommandOutcomeUncertain);

        wire.StatusReport[43] = kind == "scan" ? (byte)2 : (byte)0;
        if (kind == "scan")
            BinaryPrimitives.WriteUInt16LittleEndian(wire.StatusReport.AsSpan(44), 2);
        else
        {
            wire.StatusReport[2] = 5;
            wire.StatusReport[5] |= 128;
            wire.StatusReport[6] = 192;
            wire.StatusReport[7] = 168;
            wire.StatusReport[8] = 1;
            wire.StatusReport[9] = 2;
            wire.StatusReport[47] = 2;
        }
        Assert.Null(wire.Hid.ReconcileOutcome(null, pending));
        Assert.True(wire.Hid.CommandOutcomeUncertain);

        if (kind == "scan")
            BinaryPrimitives.WriteUInt16LittleEndian(wire.StatusReport.AsSpan(44), 1);
        else
            wire.StatusReport[47] = 1;
        Assert.NotNull(wire.Hid.ReconcileOutcome(null, pending)?.Wireless);
        Assert.False(wire.Hid.CommandOutcomeUncertain);
    }

    [Fact]
    public void WirelessPollReadsConfigurationOnceUntilASave()
    {
        using var wire = new Wire();
        wire.Hid.ReadConfiguration();
        wire.Hid.ReadWireless();
        wire.Hid.ReadWireless();
        Assert.Equal(1, wire.ConfigurationReads);
        wire.Hid.SetWireless(Settings());
        wire.Hid.ReadWireless();
        Assert.Equal(2, wire.ConfigurationReads);
    }

    [Fact]
    public async Task HttpPreservesRawSsidAndSanitizesResponses()
    {
        var dashboard = new WirelessMonitor();
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.ConfigureHttpJsonOptions(options => DashboardJson.Configure(options.SerializerOptions));
        builder.Services.AddSingleton<IDashboardBoards>(new OneBoard(dashboard));
        await using var app = builder.Build();
        app.UseApiErrors();
        app.MapWirelessEndpoints();
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        // Endpoints validate once; PicoForgeHid trusts them, so a bad password must never reach the monitor.
        foreach (var password in new[] { "1234567", "line\nbreak", new string('1', 64) })
        {
            var rejected = await client.PostAsJsonAsync("/api/wireless/test?board=board-a",
                new { ssid = "Pico", password });
            Assert.Equal(HttpStatusCode.BadRequest, rejected.StatusCode);
        }
        Assert.Equal(0, dashboard.Tests);
        foreach (var body in new[] { "null", "[]", "{}", "{\"password\":\"secret\\n12\"}" })
        {
            var response = await client.PostAsync("/api/wireless?board=board-a",
                new StringContent(body, Encoding.UTF8, "application/json"));
            Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
            Assert.DoesNotContain("secret", await response.Content.ReadAsStringAsync());
        }
        var tested = await client.PostAsJsonAsync("/api/wireless/test?board=board-a",
            new { ssid = "escaped", ssid_hex = "4100ff", password = "secret12" });
        Assert.Equal(HttpStatusCode.OK, tested.StatusCode);
        Assert.Equal("4100ff", dashboard.SsidHex);
        Assert.DoesNotContain("secret12", await tested.Content.ReadAsStringAsync());
        var saved = await client.PostAsJsonAsync("/api/wireless?board=board-a",
            new { wifi_enabled = true, ble_enabled = true, ssid_hex = "4100ff", confirm_reboot = true,
                use_tested_wifi = true, board = new { system_clock_mhz = 200, sdcard_profile = 1, radios = 3 } });
        Assert.Equal(HttpStatusCode.OK, saved.StatusCode);
        Assert.True(dashboard.Saved!.UseTestedWifi);
        Assert.Equal("4100ff", dashboard.Saved.SsidHex);
        var contractPath = Path.Combine(SharedDashboardAssetsTests.WebRoot(), "tests/status-contract.json");
        using var contract = JsonDocument.Parse(File.ReadAllText(contractPath));
        foreach (var route in contract.RootElement.GetProperty("removed_routes").EnumerateArray())
        {
            using var request = new HttpRequestMessage(new HttpMethod(route[0].GetString()!),
                $"{route[1].GetString()}?board=board-a");
            Assert.Equal(HttpStatusCode.MethodNotAllowed, (await client.SendAsync(request)).StatusCode);
        }
        Assert.Equal(HttpStatusCode.OK, (await client.PostAsync("/api/wireless/scan?board=board-a", null)).StatusCode);
        Assert.Equal(HttpStatusCode.OK,
            (await client.PostAsync("/api/wireless/cancel?board=board-a", null)).StatusCode);
        await app.StopAsync();
    }

    private sealed class Wire : IDisposable
    {
        public PicoForgeHid Hid { get; }
        public List<byte[]> Writes { get; } = [];
        public byte[] StatusReport { get; } = Status();
        public byte ConfigurationFlags { get; set; } = 0x38;
        public bool FailConfiguration { get; set; }
        public bool StaleScanEntry { get; set; }
        public bool SaveDisconnect { get; set; }
        public bool WrongSaveMask { get; set; }
        public bool ProofReadFailure { get; set; }
        public int Reopens { get; private set; }
        public int ConfigurationReads { get; private set; }
        private byte[] result = Result(1);

        public bool NeverFinishScan { get; set; }
        public bool NeverFinishTest { get; set; }

        public Wire(TimeProvider? time = null) => Hid = new(Hardware, Read, Write, _ => Reopens++, time);

        private byte[] Read(byte id, int size)
        {
            Assert.Equal(id switch { 2 => 18, 12 => 49, 20 => 69, _ => throw new Exception("old report") }, size);
            if (id == 2)
            {
                ConfigurationReads++;
                if (FailConfiguration) throw new IOException("missing mandatory configuration");
                return Configuration(ConfigurationFlags);
            }
            if (id == 12) return StatusReport.ToArray();
            if (result[3] == 5 && SaveDisconnect) throw new IOException("save detached");
            if (result[3] == 13 && ProofReadFailure)
            {
                ProofReadFailure = false;
                throw new IOException("GET failed after signing");
            }
            return result.ToArray();
        }

        private void Write(byte[] request)
        {
            Writes.Add(request.ToArray());
            result = Result(request[3]);
            result[2] = request[2];
            switch (request[3])
            {
                case 6:
                    StatusReport[5] = (byte)((StatusReport[5] | 16) & ~128);
                    StatusReport.AsSpan(11, 32).Clear();
                    StatusReport[10] = request[5];
                    request.AsSpan(6, request[5]).CopyTo(StatusReport.AsSpan(11));
                    if (request[5] == 0) { StatusReport[5] |= 32; StatusReport[48] |= 3; }
                    break;
                case 7: StatusReport[5] = (byte)((StatusReport[5] | 32) & ~128); break;
                case 8: StatusReport[5] |= 64; break;
                case 9:
                    if (!NeverFinishTest)
                    {
                        StatusReport[47] = StatusReport[47] == 255 ? (byte)1 : (byte)(StatusReport[47] + 1);
                        StatusReport[2] = 5;
                        StatusReport[5] |= 128;
                        StatusReport[6] = 192;
                        StatusReport[7] = 168;
                        StatusReport[8] = 1;
                        StatusReport[9] = 2;
                    }
                    break;
                case 10: StatusReport[5] &= 15; StatusReport[48] = 0; break;
                case 11:
                    if (!NeverFinishScan)
                    {
                        StatusReport[43] = 2;
                        var generation = BinaryPrimitives.ReadUInt16LittleEndian(StatusReport.AsSpan(44));
                        BinaryPrimitives.WriteUInt16LittleEndian(StatusReport.AsSpan(44),
                            generation == 65535 ? (ushort)1 : (ushort)(generation + 1));
                        StatusReport[46] = 1;
                    }
                    break;
                case 12:
                    result[6] = StaleScanEntry ? (byte)2 : request[6];
                    result[7] = request[7];
                    result[8] = request[5];
                    result[9] = 3;
                    result[10] = 65;
                    result[12] = 255;
                    result[42] = unchecked((byte)-42);
                    result[43] = 4;
                    break;
                case 13: result.AsSpan(6, 16).Fill(0xab); break;
                case 5:
                    request.AsSpan(5, 4).CopyTo(result.AsSpan(6));
                    result[10] = (request[4] & 2) != 0 ? (byte)(StatusReport[5] >> 4 & 7) : (byte)0;
                    if (WrongSaveMask) result[9] ^= 4;
                    break;
            }
        }

        public void Dispose() => Hid.Dispose();
    }

    private sealed class WirelessMonitor : TestMonitor
    {
        public string? SsidHex { get; private set; }
        public int Tests { get; private set; }
        public WirelessRequest? Saved { get; private set; }
        private static WirelessStatusResponse CurrentWireless => HidProtocol.ParseWirelessStatus(Status(),
            HidProtocol.ParseConfigurationReport(Configuration(), Hardware));
        public override WirelessResponse ReadWireless(CancellationToken cancel = default) =>
            new(CurrentWireless, false);
        public override WirelessResponse SetWireless(WirelessRequest settings, CancellationToken cancel = default)
        {
            Saved = settings;
            return new(CurrentWireless, true, settings.Board);
        }
        public override IReadOnlyList<WirelessNetworkResponse> ScanWireless(CancellationToken cancel = default) => [];
        public override WirelessOperationResponse TestWireless(string ssid, string password, string? ssidHex = null,
            CancellationToken cancel = default)
        {
            SsidHex = ssidHex;
            Tests++;
            return new(5, 5, "192.168.1.2", 0, 1);
        }
        public override void CancelWirelessTest(CancellationToken cancel = default) { }
    }
}
