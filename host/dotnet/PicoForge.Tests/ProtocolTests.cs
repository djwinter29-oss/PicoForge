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
    public void SharedFilesystemVectors()
    {
        foreach (var line in File.ReadLines(Path.Combine(AppContext.BaseDirectory, "hid-filesystem.csv")).Skip(1))
        {
            var fields = line.Split(',');
            var bytes = Convert.FromHexString(fields[1]);
            if (fields[2] == "invalid")
                Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseFilesystemReport(bytes));
            else Assert.Equal(fields[2], HidProtocol.ParseFilesystemReport(bytes));
        }
    }

    [Fact]
    public void SettingsReportRoundTripsAndRejectsUnsupportedValues()
    {
        var settings = new BootSettingsResponse(200, 50, 25);
        var report = HidProtocol.BuildSettingsReport(settings);
        Assert.Equal(new byte[] { HidProtocol.SettingsReportId, 1, 200, 0, 50, 25 }, report);
        Assert.Equal(settings, HidProtocol.ParseSettingsReport(report));
        Assert.Equal(settings, HidProtocol.ParseSettingsReport([0, .. report]));
        Assert.Throws<InvalidOperationException>(() =>
            HidProtocol.BuildSettingsReport(new BootSettingsResponse(100, 50, 25)));
        report[1] = 2;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseSettingsReport(report));
    }

    [Fact]
    public async Task SnapshotDoesNotAcquireDeviceHandleLock()
    {
        using var monitor = new DeviceMonitor();
        var deviceGate = typeof(DeviceMonitor).GetField("deviceGate",
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.GetValue(monitor)!;
        using var held = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var holder = Task.Run(() =>
        {
            lock (deviceGate)
            {
                held.Set();
                release.Wait();
            }
        });
        try
        {
            await Task.Run(() => held.Wait()).WaitAsync(TimeSpan.FromSeconds(2));
            var read = await Task.Run(() => monitor.Snapshot).WaitAsync(TimeSpan.FromSeconds(2));
            Assert.False(read.Connected);
        }
        finally
        {
            release.Set();
            await holder;
        }
    }

    [Fact]
    public async Task CommandWritesDoNotBlockSnapshotAndPublishState()
    {
        foreach (var command in new Func<DeviceMonitor, object?>[]
        {
            m => m.SetLed(true),
            m => m.SetMsc(true),
            m => m.SetSettings(new BootSettingsResponse(150, 0, 0)),
            m => m.SetSelfTest("start", 1),
        })
        {
            using var monitor = new DeviceMonitor();
            var client = new BlockingCommands();
            var flags = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
            typeof(DeviceMonitor).GetField("activeClient", flags)!.SetValue(monitor, client);
            typeof(DeviceMonitor).GetField("snapshot", flags)!.SetValue(monitor, new StatusResponse(true, null, null,
                null, null, new BoardStatusResponse(false, null, true, false), null,
                new MscStatusResponse(false, false, false),
                new SelfTestStatusResponse(0, "idle", 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false),
                Settings: new BootSettingsResponse(0, 0, 0)));
            var call = Task.Run(() => command(monitor));
            try
            {
                await Task.Run(() => client.Entered.Wait()).WaitAsync(TimeSpan.FromSeconds(2));
                var read = await Task.Run(() => monitor.Snapshot).WaitAsync(TimeSpan.FromSeconds(2));
                Assert.True(read.Connected);
            }
            finally
            {
                client.Release.Set();
            }
            Assert.NotNull(await call);
            Assert.NotEqual(false, await call);
        }
    }

    private sealed class BlockingCommands : IHidCommands
    {
        public ManualResetEventSlim Entered { get; } = new();
        public ManualResetEventSlim Release { get; } = new();
        private void Block() { Entered.Set(); Release.Wait(); }
        public void SetLed(bool on) => Block();
        public void SetMsc(bool enabled, bool force = false) => Block();
        public BootSettingsResponse? ReadSettings() => new BootSettingsResponse(0, 0, 0);
        public BootSettingsResponse SetSettings(BootSettingsResponse settings) { Block(); return settings; }
        public void SetSelfTest(string action, uint sizeMib) => Block();
    }

    [Fact]
    public void RejectsUnsupportedMemoryVersionsAndModes()
    {
        var report = new byte[30];
        report[1] = 3;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseMemoryReport(report));
        report[1] = 4;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseMemoryReport(report));
        report[1] = 2;
        report[4] = 2;
        report[5] = 2;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseMemoryReport(report));
        report[4] = 1;
        report[5] = 3;
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
        var report = new byte[26];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.StatusReportVersion;
        report[2] = 9;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(3), 123_456);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 150_000_000);
        report[11] = 0x1f;
        BinaryPrimitives.WriteInt16LittleEndian(report.AsSpan(12), 2345);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 1234);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(16), 5678);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(18), 65536);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(22), 532480);

        var runtime = HidProtocol.ParseStatusReport(report);
        Assert.Equal(new RuntimeStatusResponse(9, 123_456, 150_000_000, true, true,
            true, 2345, true, true, 1234, 5678, 65536, 532480), runtime);
        Assert.Equal(new BoardStatusResponse(true, 2345, true, true), runtime.ToBoardStatus());

        report[11] = 0x80;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
    }

    [Fact]
    public void ParsesPerCoreRuntimeUsageAndRejectsNonV1StatusLayouts()
    {
        var report = new byte[26];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.StatusReportVersion;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 250_000_000);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 1234);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(16), 5678);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(18), 65536);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(22), 532480);

        var runtime = HidProtocol.ParseStatusReport(report);
        Assert.Equal((ushort)1234, runtime.CpuUsageCore0BasisPoints);
        Assert.Equal((ushort)5678, runtime.CpuUsageCore1BasisPoints);
        Assert.Equal(65536u, runtime.RamUsedBytes);
        Assert.Equal(532480u, runtime.RamTotalBytes);

        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report.AsSpan(0, 25)));
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport([.. report, 1]));
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), ushort.MaxValue);
        Assert.Null(HidProtocol.ParseStatusReport(report).CpuUsageCore0BasisPoints);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 1234);
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 10001);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(14), 1234);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(22), 0);
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        foreach (byte version in new byte[] { 2, 3, 4 })
        {
            report[1] = version;
            Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        }
    }

    [Fact]
    public void RejectsDynamicLedOnStatusWhenLedUnavailable()
    {
        var report = new byte[26];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.StatusReportVersion;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 125_000_000);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(22), 270336);
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
        Assert.Equal(1234, runtime.GetProperty("cpu_usage_core0_basis_points").GetInt32());
        Assert.Equal(5678, runtime.GetProperty("cpu_usage_core1_basis_points").GetInt32());
        Assert.Equal(65536, runtime.GetProperty("ram_used_bytes").GetInt32());
        Assert.Equal(532480, runtime.GetProperty("ram_total_bytes").GetInt32());
        var settings = snapshot.RootElement.GetProperty("settings");
        Assert.Equal(0, settings.GetProperty("system_clock_mhz").GetInt32());
        Assert.True(snapshot.RootElement.GetProperty("settings_supported").GetBoolean());
        var openApi = await client.GetStringAsync("/openapi/v1.json");
        Assert.Contains("/api/self-test", openApi);
        Assert.Contains("/api/msc/force-return", openApi);
        var forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = false });
        Assert.Equal(HttpStatusCode.BadRequest, forceResponse.StatusCode);
        Assert.False(monitor.ForcedReturn);
        var settingsResponse = await client.PostAsJsonAsync("/api/settings", new
        {
            system_clock_mhz = 200, sdio_bus_mhz = 50, spi_bus_mhz = 25
        });
        Assert.Equal(HttpStatusCode.OK, settingsResponse.StatusCode);
        using var settingsBody = JsonDocument.Parse(await settingsResponse.Content.ReadAsStringAsync());
        Assert.Equal(200, settingsBody.RootElement.GetProperty("settings").GetProperty("system_clock_mhz").GetInt32());
        Assert.Equal(new BootSettingsResponse(200, 50, 25), monitor.LastSettings);
        settingsResponse = await client.PostAsJsonAsync("/api/settings", new
        {
            system_clock_mhz = 100, sdio_bus_mhz = 50, spi_bus_mhz = 25
        });
        Assert.Equal(HttpStatusCode.BadRequest, settingsResponse.StatusCode);
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
        settingsResponse = await client.PostAsJsonAsync("/api/settings", new
        {
            system_clock_mhz = 150, sdio_bus_mhz = 0, spi_bus_mhz = 0
        });
        Assert.Equal(HttpStatusCode.Conflict, settingsResponse.StatusCode);
        forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.Conflict, forceResponse.StatusCode);
        response = await client.PostAsJsonAsync("/api/self-test", new { action = "cancel" });
        Assert.Equal(HttpStatusCode.Conflict, response.StatusCode);
        monitor.ExpectedError = new TimeoutException("HID timeout");
        settingsResponse = await client.PostAsJsonAsync("/api/settings", new
        {
            system_clock_mhz = 150, sdio_bus_mhz = 0, spi_bus_mhz = 0
        });
        Assert.Equal(HttpStatusCode.ServiceUnavailable, settingsResponse.StatusCode);
        forceResponse = await client.PostAsJsonAsync("/api/msc/force-return", new { confirm_data_loss = true });
        Assert.Equal(HttpStatusCode.ServiceUnavailable, forceResponse.StatusCode);
        response = await client.PostAsJsonAsync("/api/self-test", new { action = "cancel" });
        Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
        await app.StopAsync();
    }

    [Fact]
    public async Task LedAndMscEndpointsRequireExplicitBooleansAndMapDeliveryFailures()
    {
        var monitor = new SelfTestMonitor();
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.ConfigureHttpJsonOptions(options => options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
        builder.Services.AddSingleton<IDeviceMonitor>(monitor);
        await using var app = builder.Build();
        app.MapDeviceStatus();
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        foreach (var (path, field) in new[] { ("/api/led", "on"), ("/api/msc", "enabled") })
        {
            foreach (var body in new[] { "{}", $"{{\"{field}\":null}}", $"{{\"{field}\":\"true\"}}", $"{{\"{field}\":1}}" })
            {
                using var content = new StringContent(body, System.Text.Encoding.UTF8, "application/json");
                var bad = await client.PostAsync(path, content);
                Assert.Equal(HttpStatusCode.BadRequest, bad.StatusCode);
            }
        }
        Assert.Null(monitor.LastLed);
        var response = await client.PostAsJsonAsync("/api/led", new { on = false });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.False(monitor.LastLed);
        response = await client.PostAsJsonAsync("/api/msc", new { enabled = false });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        foreach (Exception error in new Exception[] { new IOException(), new TimeoutException(), new UnauthorizedAccessException() })
        {
            monitor.ExpectedError = error;
            response = await client.PostAsJsonAsync("/api/led", new { on = true });
            Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
            response = await client.PostAsJsonAsync("/api/msc", new { enabled = true });
            Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
        }
        await app.StopAsync();
    }

    private sealed class SelfTestMonitor : IDeviceMonitor
    {
        public bool Available { get; set; } = true;
        public (string Action, uint Size) LastCommand { get; private set; }
        public Exception? ExpectedError { get; set; }
        public bool ForcedReturn { get; private set; }
        public BootSettingsResponse? LastSettings { get; private set; }
        public StatusResponse Snapshot => new(true, null, null,
            new RuntimeStatusResponse(0, 42, 250_000_000, true, false,
                false, null, false, false, 1234, 5678, 65536, 532480),
            null, null, null, null, Settings: new BootSettingsResponse(0, 0, 0));
        public bool? LastLed { get; private set; }
        public bool SetLed(bool on)
        {
            if (ExpectedError is not null) throw ExpectedError;
            LastLed = on;
            return Available;
        }
        public BootSettingsResponse? SetSettings(BootSettingsResponse settings)
        {
            if (ExpectedError is not null) throw ExpectedError;
            if (!Available) return null;
            LastSettings = settings;
            return settings;
        }
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
