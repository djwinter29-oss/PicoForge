using Microsoft.AspNetCore.Http.HttpResults;
using System.Text.Json.Serialization;
using System.Text.Json;

namespace PicoForge;

internal static class StatusEndpoints
{
    public sealed record SetLedRequest(bool? On);
    public sealed record SetLedResponse(BoardStatusResponse? BoardStatus);
    /// <summary>Requested next-boot clock, SD-card profile and optional radio enables (omitted keeps saved).</summary>
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
    /// <summary>Command delivery acknowledgement; completion comes from HID telemetry.</summary>
    public sealed record SelfTestAcceptedResponse(bool Accepted);
    /// <summary>Explicit acknowledgement that the device will reboot and disconnect.</summary>
    public sealed record RebootRequest(bool Confirm);

    public static IEndpointRouteBuilder MapDeviceStatus(this IEndpointRouteBuilder endpoints)
    {
        endpoints.MapGet("/api/wireless", GetWireless);
        endpoints.MapPost("/api/wireless", SetWireless);
        endpoints.MapGet("/api/boards", GetBoards)
            .WithName("GetBoards")
            .WithSummary("Discover attached PicoForge boards")
            .Produces<DashboardBoardList>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
        endpoints.MapGet("/api/status", GetStatus)
            .WithName("GetDeviceStatus")
            .WithSummary("Get current PicoForge HID status")
            .WithDescription("Returns the selected board's connection state and latest hardware/runtime reports. Pass its ID from /api/boards as the board query.")
            .Produces<StatusResponse>(StatusCodes.Status200OK);
        endpoints.MapPost("/api/led", SetLed)
            .WithName("SetBoardLed")
            .WithSummary("Set the onboard board LED")
            .Produces<SetLedResponse>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status400BadRequest)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
        endpoints.MapPost("/api/settings", SetSettings)
            .WithName("SetBootSettings")
            .WithSummary("Save persistent boot settings")
            .WithDescription("Saves supported boot profiles and verifies the HID command result. Firmware reboots automatically on success.")
            .Produces<SetSettingsResponse>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status400BadRequest)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
        endpoints.MapPost("/api/reboot", Reboot)
            .WithName("RebootDevice")
            .WithSummary("Reboot PicoForge after explicit confirmation")
            .Produces(StatusCodes.Status202Accepted)
            .ProducesProblem(StatusCodes.Status400BadRequest)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
        endpoints.MapPost("/api/msc", SetMsc)
            .WithName("SetMscMode")
            .WithSummary("Request USB mass-storage mode")
            .Produces<MscStatusResponse>(StatusCodes.Status200OK)
            .ProducesProblem(StatusCodes.Status400BadRequest)
            .ProducesProblem(StatusCodes.Status409Conflict)
            .ProducesProblem(StatusCodes.Status503ServiceUnavailable);
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

    private static IResult GetWireless(string? board, IDashboardBoards dashboard)
    {
        try { return Results.Ok(dashboard.ReadWireless(board)); }
        catch (NotSupportedException) { return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" }); }
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
        catch (NotSupportedException) { return Results.Conflict(new { error = "Wireless provisioning is unavailable on this firmware" }); }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503, title: "Wireless provisioning outcome is uncertain; command was not resent");
        }
    }

    private static IResult GetBoards(IDashboardBoards dashboard)
    {
        try { return Results.Ok(dashboard.Boards()); }
        catch (IOException error) { return Results.Problem(statusCode: 503, title: error.Message); }
        catch (UnauthorizedAccessException error) { return Results.Problem(statusCode: 503, title: error.Message); }
        catch (InvalidOperationException error) { return Results.Conflict(new { error = error.Message }); }
    }

    private static Ok<StatusResponse> GetStatus(string? board, IDashboardBoards dashboard,
        CancellationToken cancellationToken)
    {
        _ = cancellationToken;
        return TypedResults.Ok(dashboard.Snapshot(board));
    }

    private static IResult SetLed(SetLedRequest request, string? board, IDashboardBoards dashboard)
    {
        if (request.On is not bool on)
            return Results.Problem(statusCode: 400, title: "Expected JSON with a boolean 'on' field");
        try
        {
            if (!dashboard.SetLed(board, on))
                return Results.Conflict(new { error = "PicoForge HID is disconnected or the board LED is unavailable" });
            return Results.Ok(new SetLedResponse(dashboard.Snapshot(board).BoardStatus));
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503, title: "HID command delivery failed");
        }
        catch (InvalidOperationException error)
        {
            return Results.Conflict(new { error = error.Message });
        }
    }

    private static IResult SetSettings(SetSettingsRequest request, string? board, IDashboardBoards dashboard)
    {
        if (request.SystemClockMhz is not (0 or 150 or 200 or 250) ||
            request.SdCardProfile is not (0 or 1) || request.Radios is < 0 or > 3)
            return Results.Problem(statusCode: 400, title: "Unsupported boot settings value");
        var settings = new BootSettingsResponse(request.SystemClockMhz.Value, request.SdCardProfile.Value,
            request.Radios ?? dashboard.Snapshot(board).Settings?.Radios ?? 0);
        try
        {
            if (request.ConfirmDataLoss)
            {
                var status = dashboard.Snapshot(board);
                if (!status.Connected || status.CommandOutcomeUncertain ||
                    (status.Hardware?.Capabilities & HidProtocol.CapabilityForcedSave) !=
                        HidProtocol.CapabilityForcedSave ||
                    status.Msc is not { Enabled: true, TransitionPending: false } || status.SelfTest?.Active == true)
                    return Results.Conflict(new { error = "Forced save is unavailable or storage is busy" });
            }
            var saved = dashboard.SetSettings(board, settings, request.ConfirmDataLoss);
            return saved is null
                ? Results.Conflict(new { error = "Boot settings are unavailable or storage is busy" })
                : Results.Ok(new SetSettingsResponse(saved));
        }
        catch (InvalidOperationException error)
        {
            return Results.Conflict(new { error = request.ConfirmDataLoss &&
                dashboard.Snapshot(board).CommandOutcomeUncertain
                    ? $"Forced save outcome is uncertain: {error.Message}" : error.Message });
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503, title: request.ConfirmDataLoss
                ? "Forced save outcome is uncertain; the command was not resent"
                : "HID settings command delivery failed");
        }
        catch (NotSupportedException)
        {
            return Results.Problem(statusCode: 503, title: "HID settings command is not supported");
        }
    }

    private static IResult Reboot(RebootRequest request, string? board, IDashboardBoards dashboard)
    {
        if (!request.Confirm)
            return Results.Problem(statusCode: 400, title: "Explicit reboot confirmation is required");
        try
        {
            return dashboard.Reboot(board)
                ? Results.Accepted()
                : Results.Conflict(new { error = "PicoForge is disconnected or storage is busy" });
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503, title: "HID reboot command delivery failed");
        }
        catch (InvalidOperationException error)
        {
            return Results.Conflict(new { error = error.Message });
        }
    }

    private static IResult SetMsc(SetMscRequest request, string? board, IDashboardBoards dashboard)
    {
        if (request.Enabled is not bool enabled)
            return Results.Problem(statusCode: 400, title: "Expected JSON with a boolean 'enabled' field");
        try
        {
            var status = dashboard.SetMsc(board, enabled);
            return status is null
                ? Results.Conflict(new { error = "PicoForge HID is disconnected" })
                : Results.Ok(status);
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return Results.Problem(statusCode: 503, title: "HID command delivery failed");
        }
        catch (InvalidOperationException error)
        {
            return Results.Conflict(new { error = error.Message });
        }
    }

    private static Results<Ok<SetMscResponse>, ProblemHttpResult> ForceReturnMsc(
        ForceReturnMscRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!request.ConfirmDataLoss)
            return TypedResults.Problem(statusCode: 400, title: "Explicit confirm_data_loss: true is required");
        try
        {
            var status = dashboard.SetMsc(board, false, force: true);
            return status is null
                ? TypedResults.Problem(statusCode: 409, title: "Enabled MSC with force-return support is unavailable")
                : TypedResults.Ok(new SetMscResponse(status));
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return TypedResults.Problem(statusCode: 503, title: "HID command delivery failed");
        }
        catch (InvalidOperationException error)
        {
            return TypedResults.Problem(statusCode: 409, title: error.Message);
        }
    }

    private static Results<Accepted<SelfTestAcceptedResponse>, ProblemHttpResult> SetSelfTest(
        SetSelfTestRequest request, string? board, IDashboardBoards dashboard,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (request.Action is not ("start" or "cancel") ||
            (request.Action == "start" && request.SizeMib is not (>= 1 and <= 1024)))
            return TypedResults.Problem(statusCode: 400, title: "Expected start/cancel and a size from 1 to 1024 MiB");
        try
        {
            if (!dashboard.SetSelfTest(board, request.Action, request.SizeMib))
                return TypedResults.Problem(statusCode: 409, title: "PicoForge HID self-test interface is unavailable");
            return TypedResults.Accepted("/api/status", new SelfTestAcceptedResponse(true));
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            return TypedResults.Problem(statusCode: 503, title: "HID command delivery failed");
        }
        catch (InvalidOperationException error)
        {
            return TypedResults.Problem(statusCode: 409, title: error.Message);
        }
    }
}