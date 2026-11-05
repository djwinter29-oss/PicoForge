using System.Text.Json;
using static PicoForge.StatusEndpoints;

namespace PicoForge;

internal static class WirelessEndpoints
{
    private const string Retry = "Restart the dashboard and reconnect the board before retrying.";
    private static readonly JsonSerializerOptions SnakeCase =
        new() { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };

    public sealed record WirelessTestRequest(string? Ssid, string? Password = null, string? SsidHex = null);

    /// <summary>Maps the Wi-Fi routes; <see cref="UseApiErrors"/> supplies errors and these 503 messages.</summary>
    public static IEndpointRouteBuilder MapWirelessEndpoints(this IEndpointRouteBuilder endpoints)
    {
        endpoints.MapPost("/api/wireless", SetWireless)
            .WithName("SetWireless")
            .WithSummary("Stage and atomically save Wi-Fi, Bluetooth and TCP CLI settings")
            .Produces<WirelessResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage("Wireless provisioning outcome is uncertain; command was not resent"));
        endpoints.MapPost("/api/wireless/scan", (string? board, IDashboardBoards dashboard, CancellationToken cancel) =>
                Results.Ok(new { networks = dashboard.Monitor(board).ScanWireless(cancel) }))
            .WithName("ScanWireless")
            .WithSummary("Scan for nearby Wi-Fi networks")
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage("Wireless scan failed"));
        endpoints.MapPost("/api/wireless/test", TestWireless)
            .WithName("TestWireless")
            .WithSummary("Join a Wi-Fi network as a test without saving it")
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage($"Wi-Fi test result is uncertain. {Retry}",
                $"Wi-Fi test timed out; outcome is uncertain. {Retry}"));
        // A cancel during a running test returns at once; the test sends it at its next poll.
        endpoints.MapPost("/api/wireless/cancel",
            (string? board, IDashboardBoards dashboard, CancellationToken cancel) =>
            {
                dashboard.Monitor(board).CancelWirelessTest(cancel);
                return Results.Ok(new { cancelled = true });
            })
            .WithName("CancelWirelessTest")
            .WithSummary("Cancel a Wi-Fi test and clear its test credentials")
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage("Could not clear the wireless test credentials"));
        return endpoints;
    }

    // Bad input is 400 whatever the board state, so parse before selecting the board.
    private static IResult SetWireless(JsonElement request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        var settings = WirelessRequest.Parse(request);
        return Results.Ok(dashboard.Monitor(board).SetWireless(settings, cancel));
    }

    private static IResult TestWireless(JsonElement request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        WirelessTestRequest test;
        try
        {
            test = request.Deserialize<WirelessTestRequest>(SnakeCase)
                ?? throw new ArgumentException("Expected SSID and optional password");
            if (HidProtocol.EncodeSsid(test.Ssid, test.SsidHex).Length == 0) throw new ArgumentException();
            HidProtocol.ValidatePassword(test.Password ?? "");
        }
        // The message is fixed so a rejected password never echoes back.
        catch (Exception error) when (error is JsonException or ArgumentException)
        {
            throw new ArgumentException("Invalid SSID or password");
        }
        return Results.Ok(new
        {
            test = dashboard.Monitor(board).TestWireless(test.Ssid ?? "", test.Password ?? "", test.SsidHex, cancel)
        });
    }
}
