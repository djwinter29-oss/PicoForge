using System.Text.Json;
using Microsoft.Extensions.FileProviders;
using PicoForge;

var builder = WebApplication.CreateBuilder(args);
builder.Logging.AddFilter("Microsoft.AspNetCore", LogLevel.Warning);
builder.WebHost.UseUrls("http://127.0.0.1:8766");
builder.Services.ConfigureHttpJsonOptions(options =>
    options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
builder.Services.AddOpenApi();
builder.Services.AddProblemDetails();
builder.Services.AddSingleton<DashboardBoards>();
builder.Services.AddSingleton<IDashboardBoards>(services => services.GetRequiredService<DashboardBoards>());

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

var webRoot = Path.Combine(AppContext.BaseDirectory, "wwwroot");
var files = new PhysicalFileProvider(webRoot);
app.UseDefaultFiles(new DefaultFilesOptions { FileProvider = files });
app.UseStaticFiles(new StaticFileOptions { FileProvider = files });
app.MapDeviceStatus();
if (app.Environment.IsDevelopment()) app.MapOpenApi();
await app.RunAsync();