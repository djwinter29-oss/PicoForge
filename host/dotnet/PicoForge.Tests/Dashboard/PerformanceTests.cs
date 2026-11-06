// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

using System.Diagnostics;
using System.Net.Http.Json;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;
using Xunit.Abstractions;

namespace PicoForge.Tests;

/// <summary>
/// Hot-path timings for the docs' .NET baseline. Generous ceilings ensure the check only fails on
/// a real regression, not on a slow CI host.
/// </summary>
[Trait("Category", "Performance")]
public sealed class PerformanceTests(ITestOutputHelper output)
{
    [Fact]
    public async Task StatusHotPathsStayCheap()
    {
        var runtime = new RuntimeStatusResponse(1, 1, 200_000_000, true, false, true, 3000, true, false, 1200, 800,
            100_000, 520_192, 1);
        var status = new StatusResponse(true, null,
            new("PicoForge", "RP2350", 2, "0001020304050607", Capabilities: 0xec), runtime, DateTimeOffset.UtcNow,
            new MemoryStatusResponse(1, true, true, true, true, "sd", "spi", 25_000_000, 1024, 524_288,
                new(0x1b, "SD", "CARD1", "2.1", 0x12345678, 2021, 4))
            {
                Msc = new(false, false, false), FilesystemFormat = "fat32"
            },
            BoardConfiguration: new(200, 1, 0, 0, 1, [2, 3, 4, 5, 255, 255], 0x38, 2, 25, 255),
            Wireless: new() { WifiEnabled = true, BleEnabled = true, WifiState = WifiState.Connected,
                BleState = BleState.Advertising, Ssid = "benchmark", SsidHex = "62656e63686d61726b",
                Ipv4 = "192.0.2.1", TcpCliEnabled = true, TcpCliStarted = true,
                NetworkConfigured = true, BleKeyConfigured = true });
        using var monitor = new DeviceMonitor(() => throw new NotSupportedException(), time: new ManualTime());
        monitor.Attach(new ForcedSaveTests.Commands(), status);
        var options = DashboardJson.Options;

        AssertPayload(JsonSerializer.SerializeToElement(monitor.Snapshot, options));
        var snapshotUs = Measure(10_000, () => _ = monitor.Snapshot);
        AssertPayload(JsonSerializer.SerializeToElement(monitor.Snapshot, options));
        var serializeUs = Measure(10_000, () => _ = JsonSerializer.SerializeToUtf8Bytes(monitor.Snapshot, options));
        AssertPayload(JsonSerializer.SerializeToElement(monitor.Snapshot, options));

        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton<IDashboardBoards>(new OneBoard(monitor));
        await using var app = await DashboardApplication.BuildAsync(builder);
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        for (var i = 0; i < 100; i++) await client.GetFromJsonAsync<JsonElement>("/api/status?board=board-a");
        AssertPayload(await client.GetFromJsonAsync<JsonElement>("/api/status?board=board-a"));
        var started = Stopwatch.GetTimestamp();
        const int requests = 1000;
        for (var i = 0; i < requests; i++) await client.GetByteArrayAsync("/api/status?board=board-a");
        var requestUs = Stopwatch.GetElapsedTime(started).TotalMicroseconds / requests;
        AssertPayload(await client.GetFromJsonAsync<JsonElement>("/api/status?board=board-a"));
        await app.StopAsync();

        output.WriteLine($"Snapshot getter: {snapshotUs:F2} us; status JSON: {serializeUs:F1} us; " +
            $"GET /api/status: {requestUs:F0} us");
        Assert.InRange(snapshotUs, 0, 50);
        Assert.InRange(serializeUs, 0, 500);
        Assert.InRange(requestUs, 0, 5000);
    }

    private static void AssertPayload(JsonElement payload)
    {
        foreach (var section in new[] { "runtime", "memory", "board_configuration", "wireless" })
            Assert.Equal(JsonValueKind.Object, payload.GetProperty(section).ValueKind);
        foreach (var flag in new[] { "telemetry_stale", "storage_stale" })
            Assert.False(payload.GetProperty(flag).GetBoolean());
        Assert.Equal(JsonValueKind.Null, payload.GetProperty("wireless_error").ValueKind);
    }

    private static double Measure(int iterations, Action action)
    {
        for (var i = 0; i < iterations / 10; i++) action();
        var started = Stopwatch.GetTimestamp();
        for (var i = 0; i < iterations; i++) action();
        return Stopwatch.GetElapsedTime(started).TotalMicroseconds / iterations;
    }
}
