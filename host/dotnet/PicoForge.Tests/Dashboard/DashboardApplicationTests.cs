using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;

namespace PicoForge.Tests;

public sealed class DashboardApplicationTests
{
    [Fact]
    public async Task ProductionPipelineServesTemplatesAssetsHeadersAndJsonBodyLimits()
    {
        var boards = new StubBoards();
        await using var app = await Build(boards);
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        foreach (var route in new[] { "/", "/console", "/js/app.mjs", "/favicon.svg" })
        {
            using var response = await client.GetAsync(route);
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            Assert.Equal("nosniff", response.Headers.GetValues("X-Content-Type-Options").Single());
            Assert.Equal("DENY", response.Headers.GetValues("X-Frame-Options").Single());
            Assert.Contains("frame-ancestors 'none'",
                response.Headers.GetValues("Content-Security-Policy").Single());
            Assert.True(response.Headers.CacheControl?.NoStore);
            if (route is "/" or "/console")
            {
                var page = await response.Content.ReadAsStringAsync();
                Assert.Contains("| .NET", page);
                Assert.DoesNotContain("{{", page);
            }
        }
        foreach (var route in new[] { "/index.html", "/console.html", "/src/js/app.mjs" })
            Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync(route)).StatusCode);
        using (var foreign = new HttpRequestMessage(HttpMethod.Post, "/api/led?board=a"))
        {
            foreign.Headers.Add("Origin", "http://example.invalid");
            foreign.Content = JsonContent.Create(new { on = true });
            Assert.Equal(HttpStatusCode.Forbidden, (await client.SendAsync(foreign)).StatusCode);
        }
        foreach (var chunked in new[] { false, true })
        foreach (var (route, json, size) in new[]
        {
            ("/api/console", "{}", 2049),
            ("/api/console", "{}", 4096),
            ("/api/settings", "{\"system_clock_mhz\":200,\"sdcard_profile\":1,\"radios\":0}", 4097)
        })
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, route + "?board=a")
            {
                Content = new StringContent(json.PadRight(size), Encoding.UTF8, "application/json")
            };
            request.Headers.TransferEncodingChunked = chunked;
            using var response = await client.SendAsync(request);
            Assert.Equal(HttpStatusCode.RequestEntityTooLarge, response.StatusCode);
            Assert.True((await response.Content.ReadFromJsonAsync<JsonElement>()).TryGetProperty("error", out _));
        }
        Assert.Equal(0, boards.Lookups);
        using var valid = new StringContent(
            "{\"system_clock_mhz\":200,\"sdcard_profile\":1,\"radios\":0}".PadRight(4096),
            Encoding.UTF8, "application/json");
        Assert.Equal(HttpStatusCode.NotFound, (await client.PostAsync("/api/settings?board=a", valid)).StatusCode);
        Assert.Equal(1, boards.Lookups);
        await app.StopAsync();
    }

    [Fact]
    public async Task MalformedCommandsPrecedeBoardStateAndNeverInvokeAMonitor()
    {
        var boards = new StubBoards();
        await using var app = await Build(boards);
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        using var shared = JsonDocument.Parse(File.ReadAllText(
            Path.Combine(AppContext.BaseDirectory, "host-invalid-requests.json")));
        foreach (var state in new Exception?[]
        {
            new BoardNotFoundException("unknown"), new IOException("disconnected"),
            new BoardStateException("busy"), null
        })
        {
            boards.Failure = state;
            foreach (var (route, json) in new[]
            {
                ("/api/control/resync", "{\"confirm\":false}"),
                ("/api/settings", "{\"system_clock_mhz\":1,\"sdcard_profile\":1,\"radios\":0}"),
                ("/api/self-test", "{\"action\":\"start\",\"size_mib\":0}"),
                ("/api/wireless/test", "{\"ssid\":\"Pico\",\"password\":\"short\"}"),
                ("/api/led", "{\"on\":\"yes\"}")
            })
            {
                using var body = new StringContent(json, Encoding.UTF8, "application/json");
                using var response = await client.PostAsync(route + "?board=a", body);
                Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
                Assert.True((await response.Content.ReadFromJsonAsync<JsonElement>()).TryGetProperty("error", out _));
            }
            foreach (var invalid in shared.RootElement.EnumerateArray())
            {
                var route = invalid.GetProperty("route").GetString();
                using var response = await client.PostAsJsonAsync(route + "?board=a",
                    invalid.GetProperty("body"));
                Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
                var error = (await response.Content.ReadFromJsonAsync<JsonElement>()).GetProperty("error").GetString();
                Assert.False(string.IsNullOrEmpty(error));
                if (route == "/api/wireless/test") Assert.Equal("Invalid SSID or password", error);
            }
        }
        Assert.Equal(0, boards.Lookups);
        await app.StopAsync();
    }

    [Fact]
    public async Task KnownWirelessCancellationReturnsConflictNotServerFault()
    {
        var commands = new ForcedSaveTests.Commands();
        await using var monitor = new DeviceMonitor();
        monitor.Attach(commands, new(true, null,
            new("PicoForge", "RP2350", 2, "0001020304050607", Capabilities: 0x89),
            BoardConfiguration: new(200, 1, 0, 0, 0, [], 0, 0, 0, 0),
            Memory: new MemoryStatusResponse(1, true, true, true, false, null, null, null, null, null, null)
                { Msc = new(false, false, false) }));
        await using var app = await Build(new OneBoard(monitor));
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        var testing = client.PostAsJsonAsync("/api/wireless/test?board=board-a",
            new { ssid = "Pico", password = "password" });
        try
        {
            Assert.True(commands.TestStarted.Wait(TimeSpan.FromSeconds(2)));
            var cancel = await client.PostAsJsonAsync("/api/wireless/cancel?board=board-a", new { });
            Assert.Equal(HttpStatusCode.OK, cancel.StatusCode);
            var result = await testing.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Equal(HttpStatusCode.Conflict, result.StatusCode);
            Assert.Contains("cancelled", (await result.Content.ReadFromJsonAsync<JsonElement>())
                .GetProperty("error").GetString());
            Assert.False(monitor.Snapshot.CommandOutcomeUncertain);
            Assert.Equal(1, commands.Cancels);
        }
        finally
        {
            monitor.CancelWirelessTest();
            await testing;
            await app.StopAsync();
        }
    }

    private static Task<WebApplication> Build(IDashboardBoards boards)
    {
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton(boards);
        return DashboardApplication.BuildAsync(builder);
    }

    private sealed class StubBoards : IDashboardBoards
    {
        public int Lookups;
        public Exception? Failure = new BoardNotFoundException("unknown");
        public DashboardBoardList Boards() => new([], null, []);
        public StatusResponse Snapshot(string? id) => new(false, null);
        public IDeviceMonitor Monitor(string? id)
        {
            Interlocked.Increment(ref Lookups);
            if (Failure is not null) throw Failure;
            return new TestMonitor();
        }
    }
}
