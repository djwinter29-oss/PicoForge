using System.Text.Json;
using System.Text.Json.Serialization;

namespace PicoForge;

internal static class StatusEndpoints
{
    public sealed record SetLedRequest(bool? On);
    /// <summary>Requested next-boot clock, SD-card profile and radio enables.</summary>
    public sealed record SetSettingsRequest(int? SystemClockMhz,
        [property: JsonPropertyName("sdcard_profile")] int? SdCardProfile, bool ConfirmDataLoss = false,
        int? Radios = null);
    /// <summary>Settings values confirmed by the device after saving.</summary>
    public sealed record SetSettingsResponse(BootSettingsResponse Settings);
    public sealed record SetMscRequest(bool? Enabled);
    /// <summary>Explicit acknowledgement that forced detach may lose data and corrupt the filesystem.</summary>
    public sealed record ForceReturnMscRequest(bool ConfirmDataLoss);
    /// <summary>MSC ownership and asynchronous transition status after command delivery.</summary>
    public sealed record SetMscResponse(MscStatusResponse Msc);
    /// <summary>Start a 1..1024 MiB temporary-file test or request cancellation.</summary>
    public sealed record SetSelfTestRequest(string Action, uint SizeMib = 32);
    public sealed record CommandResyncRequest(bool Confirm);
    /// <summary>Explicit acknowledgement that the device will reboot and disconnect.</summary>
    public sealed record RebootRequest(bool Confirm);
    /// <summary>Every <c>/api</c> error body, matching the Python server.</summary>
    public sealed record ApiError(string Error);
    /// <summary>Empty success response.</summary>
    public sealed record EmptyResponse;

    /// <summary>Rejects cross-origin non-GET <c>/api</c> requests before routing, matching the Python server.</summary>
    public static IApplicationBuilder UseSameOriginApiWrites(this IApplicationBuilder app)
    {
        return app.Use(async (context, next) =>
        {
            var request = context.Request;
            if (!HttpMethods.IsGet(request.Method) && request.Path.StartsWithSegments("/api") &&
                request.Headers.TryGetValue("Origin", out var origin) && origin != $"{request.Scheme}://{request.Host}")
            {
                context.Response.StatusCode = StatusCodes.Status403Forbidden;
                await context.Response.WriteAsJsonAsync(new ApiError("API writes must be same-origin"));
                return;
            }
            await next();
        });
    }

    /// <summary>
    /// Endpoint metadata: the 503 message that replaces transport detail when this endpoint's HID command fails,
    /// matching the Python server's per-endpoint messages.
    /// </summary>
    /// <remarks>
    /// <paramref name="WithDetail"/> appends the transport error and logs it, as the Python console does.
    /// </remarks>
    public sealed record UnavailableMessage(string Message, bool WithDetail = false);

    /// <summary>
    /// The single <c>/api</c> error mapping, matching the Python server; every error body is <c>{error}</c>.
    /// Bad input is 400, an unknown board 404, a board-state conflict or firmware refusal 409, a transport fault or
    /// malformed board data 503, and anything else a logged 500.
    /// </summary>
    public static IApplicationBuilder UseApiErrors(this IApplicationBuilder app)
    {
        var logger = app.ApplicationServices.GetRequiredService<ILoggerFactory>().CreateLogger("PicoForge.Api");
        return app.Use(async (context, next) =>
        {
            try { await next(); }
            catch (Exception error) when (!context.Response.HasStarted
                && context.Request.Path.StartsWithSegments("/api")
                && !context.RequestAborted.IsCancellationRequested)
            {
                var unavailable = context.GetEndpoint()?.Metadata.GetMetadata<UnavailableMessage>();
                var (status, message) = error switch
                {
                    BadHttpRequestException bad => (bad.StatusCode, bad.Message),
                    ApiInputException or JsonException => (400, error.Message),
                    BoardNotFoundException => (404, error.Message),
                    BoardStateException => (409, error.Message),
                    _ when unavailable is { WithDetail: true } && IsUnavailable(error) =>
                        (503, $"{unavailable.Message}: {error.Message}"),
                    _ when IsUnavailable(error) => (503, unavailable?.Message ?? error.Message),
                    _ => (500, "Internal server error")
                };
                if (status == 500) logger.LogError(error, "Unhandled API error");
                else if (status == 503 && unavailable is { WithDetail: true })
                    logger.LogWarning(error, "{Message}", unavailable.Message);
                context.Response.StatusCode = status;
                await context.Response.WriteAsJsonAsync(new ApiError(message));
            }
        });
    }

    // A request that was not aborted cannot cancel, so OperationCanceledException here is a transport timeout.
    private static bool IsUnavailable(Exception error) => error is IOException or InvalidDataException
        or TimeoutException or UnauthorizedAccessException or System.Net.Sockets.SocketException
        or OperationCanceledException;

    /// <summary>Error statuses a board command can return; every body is <see cref="ApiError"/>.</summary>
    internal static RouteHandlerBuilder Errors(this RouteHandlerBuilder builder, params int[] statusCodes)
    {
        foreach (var statusCode in statusCodes) builder.Produces<ApiError>(statusCode);
        return builder;
    }

    internal static readonly int[] CommandErrors = [400, 404, 409, 503];

    public static IEndpointRouteBuilder MapStatusEndpoints(this IEndpointRouteBuilder endpoints)
    {
        endpoints.MapGet("/api/boards", GetBoards)
            .WithName("GetBoards")
            .WithSummary("Discover attached PicoForge boards")
            .Produces<DashboardBoardList>(StatusCodes.Status200OK)
            .Errors(409, 503);
        endpoints.MapGet("/api/status", GetStatus)
            .WithName("GetDeviceStatus")
            .WithSummary("Get current PicoForge HID status")
            .WithDescription("Returns the selected board's connection state and latest hardware/runtime reports. " +
                "Pass its ID from /api/boards as the board query.")
            .Produces<StatusResponse>(StatusCodes.Status200OK)
            .Errors(404);
        endpoints.MapPost("/api/led", SetLed)
            .WithName("SetBoardLed")
            .WithSummary("Set the onboard board LED")
            .Produces<EmptyResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors);
        endpoints.MapPost("/api/settings", SetSettings)
            .WithName("SetBootSettings")
            .WithSummary("Save persistent boot settings")
            .WithDescription("Saves supported boot profiles and verifies the HID command result. " +
                "Firmware reboots automatically on success.")
            .Produces<SetSettingsResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors);
        endpoints.MapPost("/api/reboot", Reboot)
            .WithName("RebootDevice")
            .WithSummary("Reboot PicoForge after explicit confirmation")
            .Produces<EmptyResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors);
        endpoints.MapPost("/api/msc", SetMsc)
            .WithName("SetMscMode")
            .WithSummary("Request USB mass-storage mode")
            .Produces<SetMscResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors);
        endpoints.MapPost("/api/self-test", SetSelfTest)
            .WithName("RequestSdSelfTest")
            .WithSummary("Start or cancel an SD-card file benchmark")
            .WithDescription("Delivers a HID command; poll /api/status for verification, cleanup, " +
                "and average read/write metrics.")
            .Produces<EmptyResponse>(StatusCodes.Status202Accepted)
            .Errors(CommandErrors);
        endpoints.MapPost("/api/control/resync", ResyncCommand)
            .WithName("ResyncCommandOutcome")
            .WithSummary("Confirm the result of an uncertain HID command")
            .Produces<CommandResyncResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors);
        endpoints.MapPost("/api/msc/force-return", ForceReturnMsc)
            .WithName("ForceReturnMsc")
            .WithSummary("Force storage back to Pico after acknowledging possible data loss")
            .WithDescription("Bypasses host eject, not device sync. " +
                "Pending includes one remount attempt; never formats or repairs.")
            .Produces<SetMscResponse>(StatusCodes.Status200OK)
            .Errors(CommandErrors);
        return endpoints;
    }

    // Endpoints own request shape, so bad input is 400 whatever the board state; the monitor owns board state.
    private static IResult GetBoards(IDashboardBoards dashboard) => Results.Ok(dashboard.Boards());

    private static IResult GetStatus(string? board, IDashboardBoards dashboard) =>
        Results.Ok(dashboard.Snapshot(board));

    private static IResult SetLed(SetLedRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        if (request.On is not bool on)
            throw new ApiInputException("Expected JSON with a boolean 'on' field");
        dashboard.Monitor(board).SetLed(on, cancel);
        return Results.Ok(new EmptyResponse());
    }

    private static IResult SetSettings(SetSettingsRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        BootSettingsResponse settings = new(request.SystemClockMhz ?? -1, request.SdCardProfile ?? -1,
            request.Radios ?? -1);
        try { HidProtocol.ValidateSettings(settings); }
        catch (ArgumentException error) { throw new ApiInputException(error.Message); }
        return Results.Ok(new SetSettingsResponse(
            dashboard.Monitor(board).SetSettings(settings, request.ConfirmDataLoss, cancel)));
    }

    private static IResult Reboot(RebootRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        if (!request.Confirm) throw new ApiInputException("Explicit reboot confirmation is required");
        dashboard.Monitor(board).Reboot(cancel);
        return Results.Ok(new EmptyResponse());
    }

    private static IResult SetMsc(SetMscRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        if (request.Enabled is not bool enabled)
            throw new ApiInputException("Expected JSON with a boolean 'enabled' field");
        return Results.Ok(new SetMscResponse(dashboard.Monitor(board).SetMsc(enabled, cancel: cancel)));
    }

    private static IResult ForceReturnMsc(ForceReturnMscRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        if (!request.ConfirmDataLoss) throw new ApiInputException("Explicit confirm_data_loss: true is required");
        return Results.Ok(new SetMscResponse(dashboard.Monitor(board).SetMsc(false, force: true, cancel)));
    }

    private static IResult SetSelfTest(SetSelfTestRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel)
    {
        if (request.Action is not ("start" or "cancel") ||
            (request.Action == "start" && request.SizeMib is not (>= 1 and <= 1024)))
            throw new ApiInputException("Expected start/cancel and a size from 1 to 1024 MiB");
        dashboard.Monitor(board).SetSelfTest(request.Action, request.SizeMib, cancel);
        return Results.Json(new EmptyResponse(), statusCode: StatusCodes.Status202Accepted);
    }

    private static IResult ResyncCommand(CommandResyncRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancel) =>
        Results.Ok(dashboard.Monitor(board).ResyncCommand(request.Confirm, cancel));
}
