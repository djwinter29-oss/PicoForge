using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PicoForge;

/// <summary>Sanitized saved configuration and live wireless state; never contains credentials.</summary>
public sealed record WirelessStatusResponse(bool WifiEnabled, bool BleEnabled, string Ssid,
    bool PasswordConfigured, bool BleKeyConfigured, string Status, byte TransactionId,
    string WifiState, string Ipv4, byte Error);

/// <summary>Wireless status with acknowledgement of an imminent reboot and, after a save, the confirmed board settings.</summary>
public sealed record WirelessResponse(WirelessStatusResponse Wireless, bool RebootPending,
    BootSettingsResponse? Settings = null);

/// <summary>Atomic board-settings and provisioning input. Omitted credentials preserve the stored values.</summary>
public sealed class WirelessRequest
{
    public bool? WifiEnabled { get; set; }
    public bool? BleEnabled { get; set; }
    public string? Ssid { get; set; }
    public string? Password { get; set; }
    public string? BleKey { get; set; }
    /// <summary>Clock, SD profile and radio switches committed in the same save.</summary>
    public BootSettingsResponse? Board { get; set; }
    public bool ConfirmReboot { get; set; }
    [JsonExtensionData]
    public Dictionary<string, JsonElement>? Extra { get; set; }

    internal static WirelessRequest Parse(JsonElement json)
    {
        try
        {
            if (json.ValueKind != JsonValueKind.Object) throw new JsonException();
            var request = json.Deserialize<WirelessRequest>(new JsonSerializerOptions
            {
                PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower
            }) ?? throw new JsonException();
            if (json.TryGetProperty("password", out var password) && password.ValueKind != JsonValueKind.String ||
                json.TryGetProperty("ble_key", out var key) && key.ValueKind != JsonValueKind.String ||
                !json.TryGetProperty("board", out var board) || board.ValueKind != JsonValueKind.Object ||
                !board.TryGetProperty("system_clock_mhz", out _) || !board.TryGetProperty("sdcard_profile", out _))
                throw new JsonException();
            HidProtocol.BuildWirelessRequest(request, 1);
            return request;
        }
        catch (Exception error) when (error is JsonException or ArgumentException)
        {
            throw new ArgumentException("Invalid wireless configuration; confirm reboot and check field types and lengths");
        }
    }
}

internal static partial class HidProtocol
{
    public const byte WirelessReportId = 30;
    public const byte WirelessConfigurationReportId = 31;
    public const int WirelessFeatureSize = 49;

    public static byte[] BuildWirelessRequest(WirelessRequest settings, byte transaction)
    {
        if (!settings.ConfirmReboot || transaction == 0 || settings.WifiEnabled is null ||
            settings.BleEnabled is null || settings.Ssid is null || settings.Extra?.Count > 0 ||
            settings.Board is not { SystemClockMhz: 0 or 150 or 200 or 250, SdCardProfile: 0 or 1, Radios: >= 0 and <= 3 })
            throw new ArgumentException("Invalid wireless configuration or missing reboot confirmation");
        byte[] ssid;
        try { ssid = new UTF8Encoding(false, true).GetBytes(settings.Ssid); }
        catch (EncoderFallbackException) { throw new ArgumentException("Invalid SSID encoding"); }
        if (ssid.Length > 32 || ssid.Contains((byte)0) || settings.WifiEnabled == true && ssid.Length == 0)
            throw new ArgumentException("SSID must be at most 32 UTF-8 bytes and nonempty when Wi-Fi is enabled");
        var report = new byte[137];
        report[0] = WirelessConfigurationReportId;
        report[1] = 2;
        report[2] = transaction;
        report[3] = (byte)settings.Board.Radios;
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(134), (ushort)settings.Board.SystemClockMhz);
        report[136] = (byte)settings.Board.SdCardProfile;
        ssid.CopyTo(report, 5);
        if (settings.Password is null) report[4] |= 1;
        else
        {
            if (settings.Password.Length is < 8 or > 63 || settings.Password.Any(c => c is < ' ' or > '~'))
                throw new ArgumentException("Password must contain 8..63 printable ASCII characters");
            Encoding.ASCII.GetBytes(settings.Password).CopyTo(report, 38);
        }
        if (settings.BleKey is null) report[4] |= 2;
        else
        {
            if (settings.BleKey.Length != 64 || settings.BleKey.Any(c => !Uri.IsHexDigit(c)))
                throw new ArgumentException("BLE key must contain 64 hexadecimal characters and be nonzero");
            var key = Convert.FromHexString(settings.BleKey);
            if (key.All(value => value == 0))
                throw new ArgumentException("BLE key must contain 64 hexadecimal characters and be nonzero");
            key.CopyTo(report, 102);
        }
        return report;
    }

    public static WirelessStatusResponse ParseWirelessStatus(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, WirelessReportId, 48);
        Require(payload[0] == 1 && (payload[1] & ~3) == 0 && payload[2] <= 6 &&
            payload[4] <= 3 && payload[42] <= 1 && payload[43] <= 1 && payload[44] <= 3 &&
            payload[45..].IndexOfAnyExcept((byte)0) < 0, "Invalid wireless status report");
        var ssid = payload.Slice(9, 33);
        var nul = ssid.IndexOf((byte)0);
        Require(nul >= 0 && ssid[nul..].IndexOfAnyExcept((byte)0) < 0, "Invalid wireless SSID encoding");
        string name;
        try { name = new UTF8Encoding(false, true).GetString(ssid[..nul]); }
        catch (DecoderFallbackException) { throw new InvalidOperationException("Invalid wireless SSID encoding"); }
        return new((payload[1] & 1) != 0, (payload[1] & 2) != 0, name,
            payload[42] != 0, payload[43] != 0,
            new[] { "idle", "pending", "completed", "invalid", "unsupported", "busy", "failed" }[payload[2]],
            payload[3], new[] { "disabled", "connecting", "connected", "failed" }[payload[4]],
            $"{payload[5]}.{payload[6]}.{payload[7]}.{payload[8]}", payload[44]);
    }
}
