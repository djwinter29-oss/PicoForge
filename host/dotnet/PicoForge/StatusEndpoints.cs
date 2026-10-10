using Microsoft.AspNetCore.Http.HttpResults;

namespace PicoForge;

internal static class StatusEndpoints
{
    public sealed record SetLedRequest(bool On);
    public sealed record SetLedResponse(BoardStatusResponse? BoardStatus);

    public static IEndpointRouteBuilder MapDeviceStatus(this IEndpointRouteBuilder endpoints)
    {
        endpoints.MapGet("/api/status", GetStatus)
            .WithName("GetDeviceStatus")
            .WithSummary("Get current PicoForge HID status")
            .WithDescription("Returns connection state and the most recent hardware and runtime HID reports.")
            .Produces<StatusResponse>(StatusCodes.Status200OK);
        endpoints.MapPost("/api/led", SetLed)
            .WithName("SetBoardLed")
            .WithSummary("Set the onboard board LED")
            .Produces<SetLedResponse>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status409Conflict);
        return endpoints;
    }

    private static Ok<StatusResponse> GetStatus(IDeviceMonitor monitor, CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return TypedResults.Ok(monitor.Snapshot);
    }

    private static IResult SetLed(SetLedRequest request, IDeviceMonitor monitor)
    {
        if (!monitor.SetLed(request.On))
        {
            return Results.Conflict(new { error = "PicoForge HID is disconnected or the board LED is unavailable" });
        }
        return Results.Ok(new SetLedResponse(monitor.Snapshot.BoardStatus));
    }
}