using Microsoft.AspNetCore.Http.HttpResults;

namespace PicoForge.Host;

internal static class StatusEndpoints
{
    public static IEndpointRouteBuilder MapDeviceStatus(this IEndpointRouteBuilder endpoints)
    {
        endpoints.MapGet("/api/status", GetStatus)
            .WithName("GetDeviceStatus")
            .WithSummary("Get current PicoForge HID status")
            .WithDescription("Returns connection state and the most recent hardware and runtime HID reports.")
            .Produces<StatusResponse>(StatusCodes.Status200OK);
        return endpoints;
    }

    private static Ok<StatusResponse> GetStatus(IDeviceMonitor monitor, CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return TypedResults.Ok(monitor.Snapshot);
    }
}