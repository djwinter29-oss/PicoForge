using System.Buffers.Binary;
using PicoForge;
using Xunit;
using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;

namespace PicoForge.Tests;

public sealed class ProtocolTests
{
    [Fact]
    public void ParsesNorMemoryLayoutAndRejectsNonzeroReservedBytes()
    {
        var report = new byte[30];
        new byte[] { 3, 3, 1, 0x3f, 2, 2 }.CopyTo(report, 0);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(6), 992063);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(10), 6000);
        new byte[] { 0xef, 0x40, 0x18 }.CopyTo(report, 14);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(17), 16777216);
        var memory = HidProtocol.ParseMemoryReport(report);
        Assert.Equal("nor-flash", memory.ComponentType);
        Assert.Equal("spi", memory.Mode);
        Assert.Equal(6000ul * 512, memory.CapacityBytes);
        Assert.Equal(16777216u, memory.PhysicalCapacityBytes);
        Assert.Equal("ef4018", memory.JedecId);
        Assert.Null(memory.Manufacturer);
        report[29] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseMemoryReport(report));
    }

    [Fact]
    public void ParsesProductMcuAndSerialIdentity()
    {
        var report = CreateInfoReport();

        Assert.Equal(new HardwareInfoResponse("PicoForge", "RP2350", 2, "0123456789abcdef"),
            HidProtocol.ParseInfoReport(report));
    }

    [Fact]
    public void ParsesFirmwareReleaseAndTransportPrefixesInIdentityLayoutThree()
    {
        var report = CreateInfoReport();
        report[1] = 3;
        report[39] = 0;
        report[40] = 1;
        report[41] = 0;
        Assert.Equal("0.1.0", HidProtocol.ParseInfoReport(report).FirmwareVersion);
        Assert.Equal("0.1.0", HidProtocol.ParseInfoReport([0, .. report]).FirmwareVersion);
        Assert.Equal("0.1.0", HidProtocol.ParseInfoReport([1, .. report]).FirmwareVersion);
        Assert.Equal("0.1.0", HidProtocol.ParseInfoReport([.. report, 0]).FirmwareVersion);
        report[42] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report));
        report[42] = 0;
        report[1] = 4;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report));
    }

    [Fact]
    public void ParsesPaddedStandardFeatureReportWhenVersionEqualsReportId()
    {
        var report = new byte[44];
        CreateInfoReport().CopyTo(report, 0);

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report).ProductName);
    }

    [Fact]
    public void ParsesZeroPrefixedFeatureReport()
    {
        var report = new byte[45];
        report[0] = 0;
        report[1] = HidProtocol.InfoReportId;
        CreateInfoReport().AsSpan(1).CopyTo(report.AsSpan(2));

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report).ProductName);
    }

    [Fact]
    public void ParsesHidSharpFeatureReportWithRepeatedReportIdPrefix()
    {
        var report = new byte[44];
        report[0] = HidProtocol.InfoReportId;
        report[1] = HidProtocol.InfoReportId;
        CreateInfoReport().AsSpan(1).CopyTo(report.AsSpan(2));

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report).ProductName);
    }

    [Fact]
    public void ParsesRuntimeStateAndRejectsUnknownFlags()
    {
        var report = new byte[14];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 9;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(3), 123_456);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 150_000_000);
        report[11] = 0x1f;
        BinaryPrimitives.WriteInt16LittleEndian(report.AsSpan(12), 2345);

        var runtime = HidProtocol.ParseStatusReport(report);
        Assert.Equal(new RuntimeStatusResponse(9, 123_456, 150_000_000, true, true,
            true, 2345, true, true), runtime);
        Assert.Equal(new BoardStatusResponse(true, 2345, true, true), runtime.ToBoardStatus());

        report[11] = 0x80;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
    }

    [Fact]
    public void ParsesRuntimeUsageAndRejectsInvalidExtensions()
    {
        var report = new byte[24];
        report[0] = 2;
        report[1] = 3;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 250_000_000);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 1234);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(16), 65536);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(20), 532480);
        var runtime = HidProtocol.ParseStatusReport(report);
        Assert.Equal((ushort)1234, runtime.CpuUsageBasisPoints);
        Assert.Equal(65536u, runtime.RamUsedBytes);
        Assert.Equal(532480u, runtime.RamTotalBytes);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report.AsSpan(0, 23)));
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport([.. report, 1]));
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), ushort.MaxValue);
        Assert.Null(HidProtocol.ParseStatusReport(report).CpuUsageBasisPoints);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 10001);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(20), 0);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(20), 65535);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
    }

    [Fact]
    public void RejectsDynamicLedOnStatusWhenLedUnavailable()
    {
        var report = new byte[14];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.ReportVersion;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 125_000_000);
        report[11] = 0x10;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        Assert.Equal(new byte[] { HidProtocol.LedCommandReportId, 2, 1 },
            HidProtocol.BuildLedCommandReport(true));
        Assert.Equal(new byte[] { HidProtocol.LedCommandReportId, 2, 0 },
            HidProtocol.BuildLedCommandReport(false));
    }

    [Fact]
    public void ParsesMemoryReportAndSdCid()
    {
        var report = CreateMemoryReport(0x3f, 1, 2, 25_000_000, 4096,
            [0x1b, (byte)'S', (byte)'D', (byte)'C', (byte)'A', (byte)'R', (byte)'D', (byte)'1',
             0x21, 0x12, 0x34, 0x56, 0x78, 0x01, 0xaa, 0xff]);

        Assert.Equal(new MemoryStatusResponse(7, true, true, true, true, "sd-card", "spi",
            25_000_000, 4096, 2_097_152,
            new SdCardIdentity(0x1b, "SD", "CARD1", "2.1", 0x12345678, 2026, 10)),
            HidProtocol.ParseMemoryReport(report));
    }

    [Fact]
    public void ParsesUninitializedMemoryWithUnknownPresence()
    {
        var report = CreateMemoryReport(0x01, 1, 1, 0, 0, new byte[16]);

        var memory = HidProtocol.ParseMemoryReport(report);
        Assert.True(memory.Configured);
        Assert.False(memory.PresenceKnown);
        Assert.Null(memory.Present);
        Assert.Null(memory.CapacityBytes);
        Assert.Null(memory.Manufacturer);
    }

    [Fact]
    public void ParsesFilesystemFormatReport()
    {
        Assert.Equal("exfat", HidProtocol.ParseFilesystemReport([7, 2, 4, 4]));
        Assert.Equal("unknown", HidProtocol.ParseFilesystemReport([7, 2, 4, 0]));
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseFilesystemReport([7, 2, 4, 5]));
    }

    [Fact]
    public void RejectsInitializedMemoryWithoutKnownPresence()
    {
        var report = CreateMemoryReport(0x3d, 1, 2, 25_000_000, 4096, new byte[16]);

        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseMemoryReport(report));
    }

    [Fact]
    public void ParsesMscStatusAndBuildsToggleCommand()
    {
        Assert.Equal(new MscStatusResponse(true, false, false),
            HidProtocol.ParseMscStatusReport([5, 2, 1, 0, 0]));
           Assert.Equal(new MscStatusResponse(false, true, true),
               HidProtocol.ParseMscStatusReport([5, 2, 0, 1, 1]));
        Assert.Equal(new byte[] { HidProtocol.MscCommandReportId, 2, 1 },
            HidProtocol.BuildMscCommandReport(true));
        Assert.Equal(new byte[] { HidProtocol.MscCommandReportId, 2, 0 },
            HidProtocol.BuildMscCommandReport(false));
        Assert.Equal(new byte[] { 5, 3, 2 }, HidProtocol.BuildMscCommandReport(false, force: true));
        Assert.Throws<InvalidOperationException>(() => HidProtocol.BuildMscCommandReport(true, force: true));
        Assert.True(HidProtocol.ParseMscStatusReport([5, 3, 1, 0, 0]).ForceReturnSupported);
    }

    [Fact]
    public void RejectsUnsupportedVersionAndReservedIdentityBytes()
    {
        var report = CreateInfoReport();
        report[1] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report));

        report[1] = HidProtocol.ReportVersion;
        report[^1] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report));
    }

    private static byte[] CreateInfoReport()
    {
        var report = new byte[43];
        report[0] = HidProtocol.InfoReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 2;
        "PicoForge"u8.CopyTo(report.AsSpan(3));
        "0123456789abcdef"u8.CopyTo(report.AsSpan(23));
        return report;
    }

    [Fact]
    public void SelfTestCommandsAndTimingsAreDecoded()
    {
        Assert.Equal(new byte[] { 6, 2, 1, 32, 0, 0, 0 }, HidProtocol.BuildSelfTestCommandReport("start", 32));
        Assert.Equal(new byte[] { 6, 2, 0, 0, 0, 0, 0 }, HidProtocol.BuildSelfTestCommandReport("cancel"));
        Assert.Equal(new byte[] { 6, 2, 1, 0, 4, 0, 0 }, HidProtocol.BuildSelfTestCommandReport("start", 1024));
        Assert.Throws<InvalidOperationException>(() => HidProtocol.BuildSelfTestCommandReport("start", 1025));
        var report = new byte[33];
        report[0] = 6;
        report[1] = 2;
        report[3] = 6;
        report[4] = 2;
        foreach (var offset in new[] { 5, 9, 13 }) BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(offset), 33554432);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(17), 16000);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(21), 8000);
        var status = HidProtocol.ParseSelfTestReport(report);
        Assert.Equal("passed", status.State);
        Assert.Equal(100d, status.ProgressPercent);
        Assert.Equal(2097152d, status.WriteBytesPerSecond!.Value);
        Assert.Equal(4194304d, status.ReadBytesPerSecond!.Value);
        Assert.True(status.VolumeMounted);
        foreach (var offset in new[] { 5, 9, 13 })
            BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(offset), 1073741824);
        Assert.Equal(1073741824u, HidProtocol.ParseSelfTestReport(report).TargetBytes);
        Assert.Equal(100d, HidProtocol.ParseSelfTestReport(report).ProgressPercent);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(5), 1073741825);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSelfTestReport(report));
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(5), 1073741824);
        report[4] = 3;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSelfTestReport(report));
        report[4] = 2;
        report[31] = 11;
        Assert.Equal((byte)11, HidProtocol.ParseSelfTestReport(report).RequestError);
        report[31] = 12;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSelfTestReport(report));
        report[31] = 0;
        report[^1] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSelfTestReport(report));
    }

    [Fact]
    public async Task SelfTestEndpointValidatesAndAcknowledgesCommandDelivery()
    {
        var monitor = new SelfTestMonitor();
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.ConfigureHttpJsonOptions(options => options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
        builder.Services.AddOpenApi();
        builder.Services.AddSingleton<IDeviceMonitor>(monitor);
        await using var app = builder.Build();
        app.MapDeviceStatus();
        app.MapOpenApi();
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        using var snapshot = JsonDocument.Parse(await client.GetStringAsync("/api/status"));
        var runtime = snapshot.RootElement.GetProperty("runtime");
        Assert.Equal(1234, runtime.GetProperty("cpu_usage_basis_points").GetInt32());
        Assert.Equal(65536, runtime.GetProperty("ram_used_bytes").GetInt32());
        Assert.Equal(532480, runtime.GetProperty("ram_total_bytes").GetInt32());
        var openApi = await client.GetStringAsync("/openapi/v1.json");
        Assert.Contains("/api/self-test", openApi);
        Assert.Contains("/api/msc/force-return", openApi);
        var forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = false });
        Assert.Equal(HttpStatusCode.BadRequest, forceResponse.StatusCode);
        Assert.False(monitor.ForcedReturn);
        forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.OK, forceResponse.StatusCode);
        Assert.True(monitor.ForcedReturn);
        using var forceStatus = JsonDocument.Parse(await forceResponse.Content.ReadAsStringAsync());
        Assert.True(forceStatus.RootElement.GetProperty("msc").GetProperty("transition_pending").GetBoolean());
        var response = await client.PostAsJsonAsync("/api/self-test", new { action = "start", size_mib = 16 });
        Assert.Equal(HttpStatusCode.Accepted, response.StatusCode);
        Assert.Equal(("start", 16u), monitor.LastCommand);
        response = await client.PostAsJsonAsync("/api/self-test", new { action = "start", size_mib = 1024 });
        Assert.Equal(HttpStatusCode.Accepted, response.StatusCode);
        Assert.Equal(("start", 1024u), monitor.LastCommand);
        response = await client.PostAsJsonAsync("/api/self-test", new { action = "start", size_mib = 1025 });
        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
        monitor.Available = false;
        forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.Conflict, forceResponse.StatusCode);
        response = await client.PostAsJsonAsync("/api/self-test", new { action = "cancel" });
        Assert.Equal(HttpStatusCode.Conflict, response.StatusCode);
        monitor.ExpectedError = new TimeoutException("HID timeout");
        forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.ServiceUnavailable, forceResponse.StatusCode);
        response = await client.PostAsJsonAsync("/api/self-test", new { action = "cancel" });
        Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
        await app.StopAsync();
    }

    private sealed class SelfTestMonitor : IDeviceMonitor
    {
        public bool Available { get; set; } = true;
        public (string Action, uint Size) LastCommand { get; private set; }
        public Exception? ExpectedError { get; set; }
        public bool ForcedReturn { get; private set; }
        public StatusResponse Snapshot => new(true, null, null,
            new RuntimeStatusResponse(0, 42, 250_000_000, true, false,
                false, null, false, false, 1234, 65536, 532480),
            null, null, null, null);
        public bool SetLed(bool on) => false;
        public MscStatusResponse? SetMsc(bool enabled, bool force = false)
        {
            if (ExpectedError is not null) throw ExpectedError;
            if (!Available) return null;
            ForcedReturn = force && !enabled;
            return new MscStatusResponse(true, true, false, true);
        }
        public bool SetSelfTest(string action, uint sizeMib)
        {
            if (ExpectedError is not null) throw ExpectedError;
            LastCommand = (action, sizeMib);
            return Available;
        }
    }

    private static byte[] CreateMemoryReport(byte flags, byte component, byte mode,
        uint busClockHz, uint blocks, byte[] cid)
    {
        var report = new byte[HidProtocol.MemoryPayloadSize + 1];
        report[0] = HidProtocol.MemoryReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 7;
        report[3] = flags;
        report[4] = component;
        report[5] = mode;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(6), busClockHz);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(10), blocks);
        cid.CopyTo(report.AsSpan(14));
        return report;
    }
}