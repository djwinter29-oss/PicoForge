namespace PicoForge;

internal static class DashboardAssets
{
    public static IApplicationBuilder UseTemplateFileGuard(this IApplicationBuilder app) =>
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
}
