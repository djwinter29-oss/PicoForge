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
    public void OutputContainsCurrentSharedDashboardAsset(string relativePath)
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "host/dotnet/PicoForge.sln")))
            directory = directory.Parent;
        Assert.NotNull(directory);
        var source = Path.Combine(directory.FullName, "host/web", relativePath);
        var output = Path.Combine(AppContext.BaseDirectory, "wwwroot", relativePath);
        Assert.Equal(File.ReadAllBytes(source), File.ReadAllBytes(output));
    }
}
