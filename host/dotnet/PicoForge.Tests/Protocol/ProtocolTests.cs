using System.Buffers.Binary;
using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ProtocolTests
{
    [Fact]
    public void ParsesIdentityAndBoardConfigurationReports()
    {
        var identity = HidProtocol.ParseIdentityReport(IdentityReport());
        Assert.Equal((ushort)2, identity.ProductId);
        Assert.Equal("RP2350", identity.Mcu);
        Assert.Equal((byte)2, identity.BoardModel);
        Assert.Equal("0001020304050607", identity.SerialNumber);
        Assert.Equal((uint)520192, identity.RamTotalBytes);

        var configuration = HidProtocol.ParseConfigurationReport(ConfigurationReport(), identity);
        Assert.Equal((ushort)200, configuration.StoredSystemClockMhz);
        Assert.Equal((byte)1, configuration.StoredSdCardProfile);
        Assert.Equal(new byte[] { 10, 11, 12, 13, 14, 15 }, configuration.StorageGpios);
        Assert.Equal(new BootSettingsResponse(200, 1, 0), HidProtocol.ToBootSettings(configuration));
    }

    [Fact]
    public void RejectsInvalidIdentityAndConfigurationCrossFields()
    {
        var identity = IdentityReport();
        identity[17] = 1;
        Assert.Equal((ushort)2, HidProtocol.ParseIdentityReport(identity).ProductId);
        identity = IdentityReport();
        identity[18] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseIdentityReport(identity));

        var hardware = HidProtocol.ParseIdentityReport(IdentityReport());
        var config = ConfigurationReport();
        config[14] = 4;
        Assert.Throws<InvalidOperationException>(() =>
            HidProtocol.ParseConfigurationReport(config, hardware));
        config = ConfigurationReport();
        config[14] |= 0x10;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseConfigurationReport(config, hardware));
        var wireless = IdentityReport();
        wireless[18] |= HidProtocol.CapabilityWireless;
        Assert.Equal(new BootSettingsResponse(200, 1, 2), HidProtocol.ToBootSettings(
            HidProtocol.ParseConfigurationReport(config, HidProtocol.ParseIdentityReport(wireless))));
        config[14] = (byte)((ConfigurationReport()[14] & 7) | 0x20);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseConfigurationReport(config, hardware));
        Assert.Equal(0x20, HidProtocol.ParseConfigurationReport(config,
            HidProtocol.ParseIdentityReport(wireless)).BoardFlags & 0x20);
    }

    [Fact]
    public void IdentityAcceptsForcedSaveAndWirelessCapabilities()
    {
        var report = IdentityReport();
        report[18] |= HidProtocol.CapabilityForcedSave;
        Assert.Equal((byte)0x77, HidProtocol.ParseIdentityReport(report).Capabilities);
        report[18] |= 0x80;
        Assert.True(HidProtocol.ParseIdentityReport(report).WirelessSupported);
        report[18] |= HidProtocol.CapabilitySpi | HidProtocol.CapabilitySdio;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseIdentityReport(report));
    }

    [Theory]
    [InlineData(1, 4)]
    [InlineData(2, 3)]
    public void ConfigurationAcceptsNonGpioLedAndPreservesGpioValidation(byte mcu, byte board)
    {
        var report = IdentityReport();
        report[15] = mcu;
        report[16] = board;
        report[18] |= HidProtocol.CapabilityWireless;
        var hardware = HidProtocol.ParseIdentityReport(report);
        var config = ConfigurationReport();
        config[14] &= unchecked((byte)~2);
        config[16] = byte.MaxValue;
        Assert.Equal(byte.MaxValue, HidProtocol.ParseConfigurationReport(config, hardware).LedGpio);
        report[18] &= unchecked((byte)~HidProtocol.CapabilityLed);
        var noLed = HidProtocol.ParseIdentityReport(report);
        Assert.Equal(byte.MaxValue, HidProtocol.ParseConfigurationReport(config, noLed).LedGpio);
        Assert.Throws<InvalidOperationException>(() =>
            HidProtocol.ParseConfigurationReport(ConfigurationReport(), noLed));
    }

    [Fact]
    public void ParsesTelemetryAndValidatesSentinels()
    {
        var report = new byte[HidProtocol.TelemetryPayloadSize + 1];
        report[0] = HidProtocol.TelemetryReportId;
        report[1] = 1;
        report[2] = 1;
        BinaryPrimitives.WriteInt16LittleEndian(report.AsSpan(3), 2534);
        BinaryPrimitives.WriteUInt64LittleEndian(report.AsSpan(5), 9_876_543_210);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(13), 10000);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(15), ushort.MaxValue);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(17), 125_000_000);
        report[21] = 7;
        report[22] = 0x0f;

        var telemetry = HidProtocol.ParseTelemetryReport(report);
        Assert.Equal(9_876_543_210ul, telemetry.UptimeMilliseconds);
        Assert.Equal((ushort)10000, telemetry.CpuUsageCore0BasisPoints);
        Assert.Null(telemetry.CpuUsageCore1BasisPoints);
        Assert.Equal((short)2534, telemetry.TemperatureCentidegrees);

        report[15] = 0xfe;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseTelemetryReport(report));
    }

    [Fact]
    public void ParsesStorageStatusAndRejectsDataFromInvalidSamples()
    {
        var report = StorageReport();
        var result = HidProtocol.ParseStorageReport(report, HidProtocol.ParseIdentityReport(IdentityReport()));
        Assert.Equal("sdio", result.Memory!.Mode);
        Assert.Equal((byte)2, result.Memory.SpeedMode);
        Assert.Equal((ulong)2_097_152, result.Memory.CapacityBytes);
        Assert.Equal("exfat", result.FilesystemFormat);
        Assert.Equal(2026, result.Memory.Manufacturer!.ManufactureYear);
        Assert.Equal(10, result.Memory.Manufacturer.ManufactureMonth);

        report[4] = 0;
        report[5] = 0;
        report.AsSpan(6, 4).Clear();
        report[10] = 0;
        report.AsSpan(11, 4).Clear();
        report.AsSpan(15, 16).Clear();
        report[3] = 0x01;
        report[31] = 1;
        report[32] = 1;
        var unavailable = HidProtocol.ParseStorageReport(report,
            HidProtocol.ParseIdentityReport(IdentityReport()));
        Assert.False(unavailable.Memory!.SampleValid);
        Assert.True(unavailable.Msc!.Enabled);

        report[4] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStorageReport(report,
            HidProtocol.ParseIdentityReport(IdentityReport())));
    }

    [Fact]
    public void ParsesSelfTestAndRejectsInvalidTerminalState()
    {
        var report = new byte[HidProtocol.SelfTestPayloadSize + 1];
        report[0] = HidProtocol.SelfTestReportId;
        report[1] = 1;
        report[3] = 8;
        report[4] = 2;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(5), 1_048_576);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(9), 524_288);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(13), 262_144);
        var status = HidProtocol.ParseSelfTestReport(report);
        Assert.Equal("cancelled", status.State);
        Assert.True(status.VolumeMounted);

        report[3] = 2;
        Assert.Equal("preparing", HidProtocol.ParseSelfTestReport(report).State);
        Assert.True(HidProtocol.ParseSelfTestReport(report).Active);

        report[3] = 9;
        Assert.Equal("unknown (9)", HidProtocol.ParseSelfTestReport(report).State);
        Assert.False(HidProtocol.ParseSelfTestReport(report).Active);

        report[3] = 6;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSelfTestReport(report));
    }

    [Theory]
    [InlineData(31)]
    [InlineData(32)]
    public void SelfTestRejectsNonzeroReservedBytes(int index)
    {
        var report = new byte[33];
        report[0] = 11;
        report[1] = 1;
        report[index] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSelfTestReport(report));
    }

    [Fact]
    public void BuildsAndValidatesControlTransactionResults()
    {
        var arguments = new byte[64];
        arguments[0] = 200;
        arguments[2] = 1;
        var request = HidProtocol.BuildControlRequest(7, 5, 0, arguments);
        Assert.Equal(69, request.Length);
        Assert.Equal(new byte[] { 20, 1, 7, 5, 0, 200, 0, 1, 0, 0 }, request[..10]);
        Assert.All(request[10..], value => Assert.Equal(0, value));

        var result = new byte[HidProtocol.ControlPayloadSize + 1];
        result[0] = HidProtocol.ControlReportId;
        result[1] = 1;
        result[2] = 7;
        result[3] = 5;
        result[4] = 2;
        result[6] = 200;
        result[8] = 1;
        var parsed = HidProtocol.ParseControlResult(result);
        HidProtocol.RequireCompleted(parsed, 7, 5);
        Assert.Equal((byte)1, parsed.ResultData[2]);
        result[9] = 3;
        Assert.Equal((byte)3, HidProtocol.ParseControlResult(result).ResultData[3]);
        result[9] = 7;
        Assert.Equal((byte)7, HidProtocol.ParseControlResult(result).ResultData[3]);
        result[9] = 8;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseControlResult(result));
        result[9] = 0;

        result[4] = 4;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseControlResult(result));
    }

    [Fact]
    public async Task SettingsAreSavedSeparatelyFromConfirmedReboot()
    {
        var monitor = new EndpointMonitor();
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.ConfigureHttpJsonOptions(options =>
            options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
        builder.Services.AddOpenApi();
        builder.Services.AddSingleton<IDashboardBoards>(monitor);
        await using var app = builder.Build();
        app.MapDeviceStatus();
        app.MapOpenApi();
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };

        var saved = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0 });
        Assert.Equal(HttpStatusCode.OK, saved.StatusCode);
        Assert.Equal(new BootSettingsResponse(200, 1, 0), monitor.LastSettings);
        Assert.False(monitor.RebootRequested);

        var invalid = await client.PostAsJsonAsync("/api/settings?board=board-a",
            new { system_clock_mhz = 100, sdcard_profile = 1, radios = 0 });
        Assert.Equal(HttpStatusCode.BadRequest, invalid.StatusCode);
        var unconfirmed = await client.PostAsJsonAsync("/api/reboot?board=board-a", new { confirm = false });
        Assert.Equal(HttpStatusCode.BadRequest, unconfirmed.StatusCode);
        var reboot = await client.PostAsJsonAsync("/api/reboot?board=board-a", new { confirm = true });
        Assert.Equal(HttpStatusCode.Accepted, reboot.StatusCode);
        Assert.True(monitor.RebootRequested);

        var unknownStatus = await client.GetAsync("/api/status?board=unknown");
        Assert.Equal(HttpStatusCode.Conflict, unknownStatus.StatusCode);
        var unknownSettings = await client.PostAsJsonAsync("/api/settings?board=unknown",
            new { system_clock_mhz = 200, sdcard_profile = 1, radios = 0 });
        Assert.Equal(HttpStatusCode.Conflict, unknownSettings.StatusCode);

        using var openApi = JsonDocument.Parse(await client.GetStringAsync("/openapi/v1.json"));
        Assert.True(openApi.RootElement.GetProperty("paths").GetProperty("/api/status")
            .GetProperty("get").GetProperty("responses").TryGetProperty("409", out _));
        await app.StopAsync();
    }

    private static byte[] IdentityReport()
    {
        var report = new byte[HidProtocol.IdentityPayloadSize + 1];
        report[0] = HidProtocol.IdentityReportId;
        report[1] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(2), 2);
        for (var index = 0; index < 8; index++) report[index + 4] = (byte)index;
        report[12] = 1;
        report[13] = 2;
        report[14] = 3;
        report[15] = 2;
        report[16] = 2;
        report[18] = 0x37;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(19), 520192);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(23), 100000);
        return report;
    }

    internal static byte[] ConfigurationReport()
    {
        var report = new byte[HidProtocol.ConfigurationPayloadSize + 1];
        report[0] = HidProtocol.ConfigurationReportId;
        report[1] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(2), 200);
        report[4] = 1;
        report[5] = 0;
        report[6] = 0;
        report[7] = 1;
        report[8] = 10;
        report[9] = 11;
        report[10] = 12;
        report[11] = 13;
        report[12] = 14;
        report[13] = 15;
        report[14] = 7;
        report[15] = 2;
        report[16] = 25;
        report[17] = 8;
        return report;
    }

    private static byte[] StorageReport()
    {
        var report = new byte[HidProtocol.StoragePayloadSize + 1];
        report[0] = HidProtocol.StorageReportId;
        report[1] = 1;
        report[2] = 7;
        report[3] = 0x7f;
        report[4] = 1;
        report[5] = 1;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(6), 25_000_000);
        report[10] = 2;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(11), 4096);
        byte[] cid =
        [
            0x1b, (byte)'S', (byte)'D', (byte)'C', (byte)'A', (byte)'R', (byte)'D', (byte)'1',
            0x21, 0x12, 0x34, 0x56, 0x78, 0x01, 0xaa, 0xff
        ];
        cid.CopyTo(report, 15);
        report[31] = 4;
        return report;
    }

    private sealed class EndpointMonitor : IDashboardBoards
    {
        public BootSettingsResponse? LastSettings { get; private set; }
        public bool RebootRequested { get; private set; }
        public DashboardBoardList Boards() => new([new("board-a", "PicoForge", "uid", "/hid/a")], "board-a");
        public StatusResponse Snapshot(string? boardId)
        {
            if (boardId != "board-a")
                throw new InvalidOperationException("unknown board; rescan and select a listed board");
            return new(true, null, null, null, null, null, null, null,
                Settings: new BootSettingsResponse(0, 0, 0));
        }
        public bool SetLed(string? boardId, bool on) => true;
        public MscStatusResponse? SetMsc(string? boardId, bool enabled, bool force = false) =>
            new(enabled, false, false);
        public BootSettingsResponse? SetSettings(string? boardId, BootSettingsResponse settings, bool force = false)
        {
            LastSettings = settings;
            return settings;
        }
        public bool Reboot(string? boardId)
        {
            RebootRequested = true;
            return true;
        }
        public bool SetSelfTest(string? boardId, string action, uint sizeMib) => true;
        public WirelessResponse ReadWireless(string? boardId) => throw new NotSupportedException();
        public WirelessResponse SetWireless(string? boardId, WirelessRequest settings) => throw new NotSupportedException();
        public IReadOnlyList<WirelessNetworkResponse> ScanWireless(string? boardId) =>
            throw new NotSupportedException();
        public WirelessOperationResponse TestWireless(string? boardId, string ssid, string password,
            string? ssidHex = null) =>
            throw new NotSupportedException();
        public bool CancelWirelessTest(string? boardId) => throw new NotSupportedException();
    }
}
