using System.Text.Json;

namespace PicoForge;

public sealed record ConsoleSessionResponse(string Session);
public sealed record ConsoleReadResponse(string Data, bool Closed);
public sealed record ConsoleActionResponse(bool Ok);

internal static class ConsoleEndpoints
{
    /// <summary>
    /// Maps TCP console routes; <see cref="StatusEndpoints.UseApiErrors"/> supplies every error body.
    /// </summary>
    public static IEndpointRouteBuilder MapConsoleEndpoints(this IEndpointRouteBuilder endpoints)
    {
        var unavailable = new StatusEndpoints.UnavailableMessage("TCP console connection failed", WithDetail: true);
        var limit = new Microsoft.AspNetCore.Mvc.RequestSizeLimitAttribute(2048);
        endpoints.MapPost("/api/console", async (JsonElement payload, string? board, IDashboardBoards dashboard,
            ConsoleBridge bridge, CancellationToken cancel) =>
        {
            if (payload.ValueKind != JsonValueKind.Object || payload.EnumerateObject().Any())
                throw new ApiInputException("Expected an empty object; TCP authentication uses the connected board");
            var monitor = dashboard.Monitor(board);
            var status = monitor.Snapshot;
            if (status.WirelessCleanupRequired)
                throw new BoardStateException("Clear the Wi-Fi test before opening the console");
            if (!status.Connected || status.WirelessError is not null)
                throw new BoardStateException(status.WirelessError ?? "The board is disconnected");
            var wireless = status.Wireless ??
                throw new BoardStateException("Waiting for the board's wireless status");
            var session = await bridge.OpenAsync(wireless,
                (nonce, uid) => monitor.TcpCliAuthProof(nonce, uid, cancel), cancel);
            return Results.Ok(new ConsoleSessionResponse(session));
        })
            .WithName("OpenConsole")
            .WithSummary("Connect to the board's authenticated Wi-Fi console")
            .Produces<ConsoleSessionResponse>(StatusCodes.Status200OK)
            .Errors(StatusEndpoints.CommandErrors)
            .WithMetadata(unavailable, limit);
        endpoints.MapGet("/api/console", async (string? session, ConsoleBridge bridge, CancellationToken cancel) =>
        {
            var (data, closed) = await bridge.ReadAsync(session, cancel);
            return Results.Ok(new ConsoleReadResponse(Convert.ToBase64String(data), closed));
        })
            .WithName("ReadConsole")
            .WithSummary("Read output from the active Wi-Fi console session")
            .Produces<ConsoleReadResponse>(StatusCodes.Status200OK)
            .Errors(409, 503)
            .WithMetadata(unavailable);
        endpoints.MapPut("/api/console", (JsonElement payload, string? session, ConsoleBridge bridge) =>
        {
            if (payload.ValueKind != JsonValueKind.Object || payload.EnumerateObject().Count() != 1 ||
                !payload.TryGetProperty("data", out var field) || field.ValueKind != JsonValueKind.String)
                throw new ApiInputException("Expected base64-encoded input bytes");
            var encoded = field.GetString()!;
            if (encoded.Length > 1368) throw new ApiInputException("Input exceeds 1024 bytes");
            byte[] data;
            try { data = Convert.FromBase64String(encoded); }
            catch (FormatException error) { throw new ApiInputException(error.Message); }
            if (data.Length is < 1 or > 1024) throw new ApiInputException("Expected 1..1024 input bytes");
            bridge.Write(session, data);
            return Results.Ok(new ConsoleActionResponse(true));
        })
            .WithName("WriteConsole")
            .WithSummary("Write input bytes to the active Wi-Fi console session")
            .Produces<ConsoleActionResponse>(StatusCodes.Status200OK)
            .Errors(400, 409, 503)
            .WithMetadata(unavailable, limit);
        endpoints.MapDelete("/api/console", (string? session, ConsoleBridge bridge) =>
        {
            bridge.Close(session);
            return Results.Ok(new ConsoleActionResponse(true));
        })
            .WithName("CloseConsole")
            .WithSummary("Close the active Wi-Fi console session")
            .Produces<ConsoleActionResponse>(StatusCodes.Status200OK)
            .Errors(409);
        return endpoints;
    }
}
