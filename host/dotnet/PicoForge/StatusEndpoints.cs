using Microsoft.AspNetCore.Http.HttpResults;

namespace PicoForge;

internal static class StatusEndpoints
{
    public sealed record SetLedRequest(bool On);
    public sealed record SetLedResponse(BoardStatusResponse? BoardStatus);
    public sealed record SetMscRequest(bool Enabled);
    /// <summary>Explicit acknowledgement that forced detach may lose data and corrupt the filesystem.</summary>
    public sealed record ForceReturnMscRequest(bool ConfirmDataLoss);
    /// <summary>MSC ownership and asynchronous transition status after command delivery.</summary>
    public sealed record SetMscResponse(MscStatusResponse Msc);
    /// <summary>Start a 1..1024 MiB temporary-file test or request cancellation.</summary>
    public sealed record SetSelfTestRequest(string Action, uint SizeMib = 32);
    /// <summary>Command delivery acknowledgement; completion comes from HID telemetry.</summary>
    public sealed record SelfTestAcceptedResponse(bool Accepted);

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
        endpoints.MapPost("/api/msc", SetMsc)
            .WithName("SetMscMode")
            .WithSummary("Request USB mass-storage mode")
            .Produces<MscStatusResponse>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status409Conflict);
        endpoints.MapPost("/api/self-test", SetSelfTest)
            .WithName("RequestSdSelfTest")
            .WithSummary("Start or cancel an SD-card file benchmark")
            .WithDescription("Delivers a HID command; poll /api/status for verification, cleanup, and average read/write metrics.")
            .Produces<SelfTestAcceptedResponse>(StatusCodes.Status202Accepted)
            .ProducesProblem(StatusCodes.Status400BadRequest)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
        endpoints.MapPost("/api/msc/force-return", ForceReturnMsc)
            .WithName("ForceReturnMsc")
            .WithSummary("Force storage back to Pico after acknowledging possible data loss")
            .WithDescription("Bypasses host eject, not device sync. Pending includes one remount attempt; never formats or repairs.")
            .Produces<SetMscResponse>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status400BadRequest)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
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

    private static IResult SetMsc(SetMscRequest request, IDeviceMonitor monitor)
    {
        var status = monitor.SetMsc(request.Enabled);
        return status is null
            ? Results.Conflict(new { error = "PicoForge HID is disconnected" })
            : Results.Ok(status);
    }

    private static Results<Ok<SetMscResponse>, ProblemHttpResult> ForceReturnMsc(
        ForceReturnMscRequest request, IDeviceMonitor monitor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!request.ConfirmDataLoss)
            return TypedResults.Problem(statusCode: 400, title: "Explicit confirm_data_loss: true is required");
        try
        {
            var status = monitor.SetMsc(false, force: true);
            return status is null
                ? TypedResults.Problem(statusCode: 409, title: "Enabled MSC with force-return support is unavailable")
                : TypedResults.Ok(new SetMscResponse(status));
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return TypedResults.Problem(statusCode: 503, title: "HID command delivery failed");
        }
    }

    private static Results<Accepted<SelfTestAcceptedResponse>, ProblemHttpResult> SetSelfTest(
        SetSelfTestRequest request, IDeviceMonitor monitor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (request.Action is not ("start" or "cancel") ||
            (request.Action == "start" && request.SizeMib is not (>= 1 and <= 1024)))
            return TypedResults.Problem(statusCode: 400, title: "Expected start/cancel and a size from 1 to 1024 MiB");
        try
        {
            if (!monitor.SetSelfTest(request.Action, request.SizeMib))
                return TypedResults.Problem(statusCode: 409, title: "PicoForge HID self-test interface is unavailable");
            return TypedResults.Accepted("/api/status", new SelfTestAcceptedResponse(true));
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return TypedResults.Problem(statusCode: 503, title: "HID command delivery failed");
        }
    }
}