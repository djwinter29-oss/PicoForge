using PicoForge;

var builder = WebApplication.CreateBuilder(args);
builder.Logging.AddFilter("Microsoft.AspNetCore", LogLevel.Warning);
var port = builder.Configuration.GetValue("port", 5000);
builder.WebHost.UseUrls($"http://127.0.0.1:{port}");
await using var app = await DashboardApplication.BuildAsync(builder);
await app.RunAsync();