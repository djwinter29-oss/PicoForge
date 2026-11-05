using System.Text.Json;

namespace PicoForge;

internal static class WirelessEndpoints
{
    public sealed record WirelessTestRequest(string? Ssid, string? Password = null, string? SsidHex = null);

    public static IEndpointRouteBuilder MapWirelessEndpoints(this IEndpointRouteBuilder endpoints)
    {
        endpoints.MapGet("/api/wireless", GetWireless);
        endpoints.MapPost("/api/wireless", SetWireless);
        endpoints.MapPost("/api/wireless/scan", ScanWireless);
        endpoints.MapPost("/api/wireless/test", TestWireless);
        endpoints.MapPost("/api/wireless/cancel", CancelWirelessTest);
        return endpoints;
    }

    private static IResult GetWireless(string? board, IDashboardBoards dashboard)
    {
        try { return Results.Ok(dashboard.ReadWireless(board)); }
        catch (NotSupportedException)
        {
            return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" });
        }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503, title: "Wireless status unavailable");
        }
    }

    private static IResult SetWireless(JsonElement request, string? board, IDashboardBoards dashboard)
    {
        WirelessRequest settings;
        try { settings = WirelessRequest.Parse(request); }
        catch (ArgumentException error) { return Results.Problem(statusCode: 400, title: error.Message); }
        try { return Results.Ok(dashboard.SetWireless(board, settings)); }
        catch (NotSupportedException)
        {
            return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" });
        }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503,
                title: "Wireless provisioning outcome is uncertain; command was not resent");
        }
    }

    private static IResult ScanWireless(string? board, IDashboardBoards dashboard)
    {
        try { return Results.Ok(new { networks = dashboard.ScanWireless(board) }); }
        catch (NotSupportedException)
        {
            return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" });
        }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        { return Results.Problem(statusCode: 503, title: "Wireless scan failed"); }
    }

    private static IResult TestWireless(JsonElement request, string? board, IDashboardBoards dashboard)
    {
        WirelessTestRequest test;
        try
        {
            test = request.Deserialize<WirelessTestRequest>(new JsonSerializerOptions
            {
                PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower
            }) ?? throw new ArgumentException("Expected SSID and optional password");
            if (HidProtocol.EncodeSsid(test.Ssid, test.SsidHex).Length == 0) throw new ArgumentException();
            HidProtocol.ValidatePassword(test.Password ?? "");
        }
        catch (Exception error) when (error is JsonException or ArgumentException)
        { return Results.Problem(statusCode: 400, title: "Invalid SSID or password"); }
        try
        {
            return Results.Ok(new
            {
                test = dashboard.TestWireless(board, test.Ssid ?? "", test.Password ?? "", test.SsidHex)
            });
        }
        catch (NotSupportedException)
        {
            return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" });
        }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        { return Results.Problem(statusCode: 503, title: "Wireless connection test failed"); }
    }

    private static IResult CancelWirelessTest(string? board, IDashboardBoards dashboard)
    {
        try { return Results.Ok(new { cancelled = dashboard.CancelWirelessTest(board) }); }
        catch (NotSupportedException)
        {
            return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" });
        }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        { return Results.Problem(statusCode: 503, title: "Could not clear wireless test credentials"); }
    }
}
