using System.Reflection;
using System.Text.Json;
using Microsoft.Extensions.FileProviders;
using PicoForge;

var builder = WebApplication.CreateBuilder(args);
builder.Logging.AddFilter("Microsoft.AspNetCore", LogLevel.Warning);
var port = builder.Configuration.GetValue("port", 5000);
builder.WebHost.UseUrls($"http://127.0.0.1:{port}");
builder.Services.ConfigureHttpJsonOptions(options =>
    options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
builder.Services.AddOpenApi();
builder.Services.AddProblemDetails();
// Binding failures throw so UseApiErrors returns them as {error} like every other API error.
builder.Services.Configure<RouteHandlerOptions>(options => options.ThrowOnBadRequest = true);
builder.Services.AddSingleton<DashboardBoards>();
builder.Services.AddSingleton<IDashboardBoards>(services => services.GetRequiredService<DashboardBoards>());
builder.Services.AddSingleton<ConsoleBridge>();

var app = builder.Build();
app.UseExceptionHandler();
app.UseStatusCodePages();
app.Use(async (context, next) =>
{
    if (context.Request.Host.Host is not ("localhost" or "127.0.0.1"))
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }
    context.Response.Headers["X-Content-Type-Options"] = "nosniff";
    context.Response.Headers["X-Frame-Options"] = "DENY";
    context.Response.Headers["Referrer-Policy"] = "no-referrer";
    context.Response.Headers["Cache-Control"] = "no-store";
    context.Response.Headers["Content-Security-Policy"] =
        "default-src 'self'; connect-src 'self'; style-src 'self'; script-src 'self'; img-src 'self'; frame-ancestors 'none'";
    await next();
});
app.UseSameOriginApiWrites();
app.UseApiErrors();

var webRoot = Path.Combine(AppContext.BaseDirectory, "wwwroot");
var files = new PhysicalFileProvider(webRoot);
app.UseStaticFiles(new StaticFileOptions { FileProvider = files });
var appVersion = typeof(Program).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
    ?.InformationalVersion.Split('+')[0] ?? "0.0.0-debug";
string Page(string html) => html.Replace("{{ dashboard_type }}", ".NET").Replace("{{ app_version }}", appVersion);
var indexHtml = Page(await File.ReadAllTextAsync(Path.Combine(webRoot, "index.html")));
app.MapGet("/", () => Results.Content(indexHtml, "text/html; charset=utf-8"));
app.MapDeviceStatus();
var consoleHtml = Page(await File.ReadAllTextAsync(Path.Combine(webRoot, "console.html")));
app.MapGet("/console", () => Results.Content(consoleHtml, "text/html; charset=utf-8"));
ConsoleBridge.MapEndpoints(app);
if (app.Environment.IsDevelopment()) app.MapOpenApi();
await app.RunAsync();