using System.Reflection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.FileProviders;

namespace PicoForge;

/// <summary>Production composition shared by the executable and hardware-free integration tests.</summary>
internal static class DashboardApplication
{
    public static async Task<WebApplication> BuildAsync(WebApplicationBuilder builder)
    {
        var serial = builder.Configuration["serial"];
        var path = builder.Configuration["device-path"];
        if (serial is not null && path is not null)
            throw new ArgumentException("--serial and --device-path cannot be used together");
        builder.Services.ConfigureHttpJsonOptions(options => DashboardJson.Configure(options.SerializerOptions));
        builder.Services.AddOpenApi();
        builder.Services.Configure<RouteHandlerOptions>(options => options.ThrowOnBadRequest = true);
        builder.WebHost.ConfigureKestrel(options => options.Limits.MaxRequestBodySize = 4096);
        builder.Services.TryAddSingleton<IDashboardBoards>(services => new DashboardBoards(serial, path,
            services.GetRequiredService<ILoggerFactory>()));
        builder.Services.TryAddSingleton<ConsoleBridge>();

        var app = builder.Build();
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
            context.Response.Headers["Content-Security-Policy"] = "default-src 'self'; connect-src 'self'; " +
                "style-src 'self'; script-src 'self'; img-src 'self'; frame-ancestors 'none'";
            await next();
        });
        app.UseSameOriginApiWrites();
        app.UseApiErrors();
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
        var webRoot = Path.Combine(AppContext.BaseDirectory, "wwwroot");
        var files = new PhysicalFileProvider(webRoot);
        app.Lifetime.ApplicationStopped.Register(files.Dispose);
        app.UseStaticFiles(new StaticFileOptions { FileProvider = files });
        var version = typeof(DashboardApplication).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion.Split('+')[0] ?? "0.0.0-debug";
        string Page(string html) => html.Replace("{{ dashboard_type }}", ".NET").Replace("{{ app_version }}", version);
        var index = Page(await File.ReadAllTextAsync(Path.Combine(webRoot, "index.html")));
        var console = Page(await File.ReadAllTextAsync(Path.Combine(webRoot, "console.html")));
        app.MapGet("/", () => Results.Content(index, "text/html; charset=utf-8"));
        app.MapGet("/console", () => Results.Content(console, "text/html; charset=utf-8"));
        app.MapStatusEndpoints();
        app.MapWirelessEndpoints();
        app.MapConsoleEndpoints();
        if (app.Environment.IsDevelopment()) app.MapOpenApi();
        return app;
    }
}
