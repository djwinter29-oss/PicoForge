// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

using System.Text.Json;
using System.Text.Json.Serialization;
using static PicoForge.StatusEndpoints;

namespace PicoForge;

internal static class WirelessEndpoints
{
    private const string Retry = "Restart the dashboard and reconnect the board before retrying.";

    [JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
    public sealed record WirelessTestRequest(string? Ssid = null, string? Password = "", string? SsidHex = null);
    public sealed record WirelessScanResponse(IReadOnlyList<WirelessNetworkResponse> Networks);
    public sealed record WirelessTestResponse(WirelessOperationResponse Test);
    public sealed record WirelessCancelResponse(bool Cancelled);

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
                Results.Ok(new WirelessScanResponse(dashboard.Monitor(board).ScanWireless(cancel))))
            .WithName("ScanWireless")
            .WithSummary("Scan for nearby Wi-Fi networks")
            .Produces<WirelessScanResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage("Wireless scan failed"));
        endpoints.MapPost("/api/wireless/test", TestWireless)
            .WithName("TestWireless")
            .WithSummary("Join a Wi-Fi network as a test without saving it")
            .Produces<WirelessTestResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage($"Wi-Fi test result is uncertain. {Retry}"));
        // A cancel during a running test returns at once; the test sends it at its next poll.
        endpoints.MapPost("/api/wireless/cancel",
            (string? board, IDashboardBoards dashboard, CancellationToken cancel) =>
            {
                dashboard.Monitor(board).CancelWirelessTest(cancel);
                return Results.Ok(new WirelessCancelResponse(true));
            })
            .WithName("CancelWirelessTest")
            .WithSummary("Cancel a Wi-Fi test and clear its test credentials")
            .Produces<WirelessCancelResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors)
            .WithMetadata(new UnavailableMessage("Could not clear the wireless test credentials"));
        return endpoints;
    }

    // Bad input is 400 whatever the board state, so parse before selecting the board.
    private static IResult SetWireless(JsonElement request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        WirelessRequest settings;
        try { settings = WirelessRequest.Parse(request); }
        catch (ArgumentException error) { throw new ApiInputException(error.Message); }
        return Results.Ok(dashboard.Monitor(board).SetWireless(settings, cancel));
    }

    private static IResult TestWireless(JsonElement request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        WirelessTestRequest test;
        try
        {
            test = request.Deserialize<WirelessTestRequest>(DashboardJson.Options)
                ?? throw new ApiInputException("Expected SSID and optional password");
            if (test.Password is null || request.TryGetProperty("ssid_hex", out var hex) &&
                hex.ValueKind == JsonValueKind.Null)
                throw new ApiInputException("Invalid SSID or password");
            if (HidProtocol.EncodeSsid(test.Ssid, test.SsidHex).Length == 0)
                throw new ApiInputException("Invalid SSID or password");
            HidProtocol.ValidatePassword(test.Password ?? "");
        }
        // The message is fixed so a rejected password never echoes back.
        catch (Exception error) when (error is JsonException or ArgumentException)
        {
            throw new ApiInputException("Invalid SSID or password");
        }
        return Results.Ok(new WirelessTestResponse(
            dashboard.Monitor(board).TestWireless(test.Ssid ?? "", test.Password ?? "", test.SsidHex, cancel)));
    }
}
