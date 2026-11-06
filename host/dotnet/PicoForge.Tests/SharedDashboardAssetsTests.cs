using System.Net;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.FileProviders;
using Xunit;

namespace PicoForge.Tests;

public sealed class SharedDashboardAssetsTests
{
    [Theory]
    [InlineData("index.html")]
    [InlineData("favicon.svg")]
    [InlineData("css/dashboard.css")]
    [InlineData("js/app.mjs")]
    [InlineData("js/dashboard.mjs")]
    [InlineData("js/dashboard-api.mjs")]
    [InlineData("js/operation-state.mjs")]
    [InlineData("console.html")]
    [InlineData("css/console.css")]
    [InlineData("js/console.mjs")]
    [InlineData("js/console-terminal.mjs")]
    [InlineData("js/console-api.mjs")]
    [InlineData("js/wireless-form.mjs")]
    public void OutputContainsCurrentSharedDashboardAsset(string relativePath)
    {
        var source = Path.Combine(WebRoot(), relativePath);
        var output = Path.Combine(AppContext.BaseDirectory, "wwwroot", relativePath);
        Assert.Equal(File.ReadAllBytes(source), File.ReadAllBytes(output));
    }

    [Fact]
    public void SharedContentMappingIncludesOnlyDeployableSourceAssets()
    {
        var project = Path.Combine(WebRoot(), "../../dotnet/PicoForge/PicoForge.csproj");
        var text = File.ReadAllText(project);
        Assert.Contains("Content Include=\"../../web/src/**/*\"", text);
        Assert.DoesNotContain("Content Include=\"../../web/**/*\"", text);
    }

    [Fact]
    public void StatusJsonMatchesTheSharedPythonContract()
    {
        var path = Path.Combine(WebRoot(), "../tests/status-contract.json");
        using var contract = JsonDocument.Parse(File.ReadAllText(path));
        var empty = new StatusResponse(false, null, null, null, null, null);
        var populated = empty with
        {
            Connected = true,
            Memory = new(1, true, true, true, true, "sd", "spi", 25_000_000, 1024, 524_288, null)
            {
                Msc = new(true, false, false), FilesystemFormat = "fat32"
            },
            BoardConfiguration = new BoardConfigurationResponse(200, 1, 0, 0, 1, [2, 3, 4, 5, 255, 255], 0x38, 2, 25,
                255) { NextBoot = new(250, 0, 1) }
        };
        var options = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };
        foreach (var status in new[] { empty, populated })
        {
            var json = JsonSerializer.SerializeToElement(status, options);
            foreach (var key in contract.RootElement.GetProperty("required").EnumerateArray())
                Assert.True(json.TryGetProperty(key.GetString()!, out _), key.GetString());
            foreach (var key in contract.RootElement.GetProperty("absent").EnumerateArray())
                Assert.False(json.TryGetProperty(key.GetString()!, out _), key.GetString());
        }

        var nested = JsonSerializer.SerializeToElement(populated, options);
        foreach (var section in contract.RootElement.GetProperty("nested").EnumerateObject())
        foreach (var key in section.Value.EnumerateArray())
            Assert.True(nested.GetProperty(section.Name).TryGetProperty(key.GetString()!, out _), key.GetString());
        Assert.True(nested.GetProperty("memory").GetProperty("msc").GetProperty("enabled").GetBoolean());
        Assert.Equal("fat32", nested.GetProperty("memory").GetProperty("filesystem_format").GetString());
        var configuration = nested.GetProperty("board_configuration");
        Assert.Equal(200, configuration.GetProperty("stored_sysclk_profile").GetInt32());
        Assert.Equal(250, configuration.GetProperty("system_clock_mhz").GetInt32());
        Assert.Equal(0, configuration.GetProperty("sdcard_profile").GetInt32());
        Assert.Equal(1, configuration.GetProperty("radios").GetInt32());
        Assert.False(configuration.TryGetProperty("next_boot", out _));
    }

    [Fact]
    public async Task RawTemplatePagesAreNotServedAsStaticFiles()
    {
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        await using var app = builder.Build();
        app.Use(async (context, next) =>
        {
            if (context.Request.Path.Equals("/index.html", StringComparison.OrdinalIgnoreCase) ||
                context.Request.Path.Equals("/console.html", StringComparison.OrdinalIgnoreCase))
            {
                context.Response.StatusCode = StatusCodes.Status404NotFound;
                return;
            }
            await next();
        });
        using var files = new PhysicalFileProvider(WebRoot());
        app.UseStaticFiles(new StaticFileOptions { FileProvider = files });
        await app.StartAsync();
        using var client = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/index.html")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/console.html")).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync("/js/app.mjs")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/src/js/app.mjs")).StatusCode);
        await app.StopAsync();
    }

    internal static string WebRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "host/dotnet/PicoForge.sln")))
            directory = directory.Parent;
        Assert.NotNull(directory);
        return Path.Combine(directory.FullName, "host/web/src");
    }
}
