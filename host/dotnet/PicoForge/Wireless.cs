using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PicoForge;

/// <summary>Sanitized saved configuration and live wireless state; never contains credentials.</summary>
public sealed record WirelessStatusResponse(bool WifiEnabled, bool BleEnabled, string Ssid,
    bool PasswordConfigured, bool BleKeyConfigured, string Status, byte TransactionId,
    string WifiState, string Ipv4, byte Error, string BleState = "off", bool TcpCliStarted = false,
    bool TcpCliEnabled = true, bool TcpCliConfigurable = false, bool TcpCliAuthRequired = false,
    bool TcpCliAuthConfigurable = false, string? WifiAuthState = null, ushort ReconnectCount = 0, int? Rssi = null,
    bool RestartRequired = false);

/// <summary>Wireless status with acknowledgement of an imminent reboot and, after a save, the confirmed board settings.</summary>
public sealed record WirelessResponse(WirelessStatusResponse Wireless, bool RebootPending,
    BootSettingsResponse? Settings = null);

/// <summary>One nearby wireless access point from the sanitized scan list.</summary>
public sealed record WirelessNetworkResponse(string Ssid, int Rssi, bool Secured, string Security = "Secured");

/// <summary>Sanitized scan or temporary-join status; never includes a password.</summary>
public sealed record WirelessOperationResponse(byte TransactionId, byte State, byte Page, byte Count,
    byte WifiState, string Ipv4, IReadOnlyList<WirelessNetworkResponse> Networks, byte Detail = 0,
    ushort RemainingSeconds = 0)
{
    /// <summary>Feature 35 byte 3 while testing (states 4..6): join phase or failure reason.</summary>
    public static readonly string[] Details = ["", "Searching for network", "Checking password",
        "Waiting for IP address", "Network not found", "Password rejected", "Connected but no IP address from DHCP",
        "Timed out", "Could not start connection", "Test connection lost", "Test credentials expired"];

    public string Phase => Details[Detail];
}

/// <summary>Atomic board-settings and provisioning input. Omitted credentials preserve the stored values.</summary>
public sealed class WirelessRequest
{
    public bool? WifiEnabled { get; set; }
    public bool? BleEnabled { get; set; }
    /// <summary>Enable unauthenticated LAN CLI access; omitted preserves the saved switch.</summary>
    public bool? TcpCliEnabled { get; set; }
    /// <summary>Require the saved BLE control key to authenticate LAN CLI sessions.</summary>
    public bool? TcpCliAuthRequired { get; set; }
    public string? Ssid { get; set; }
    public string? Password { get; set; }
    public string? BleKey { get; set; }
    /// <summary>Clock, SD profile and radio switches committed in the same save.</summary>
    public BootSettingsResponse? Board { get; set; }
    /// <summary>Use the successful RAM-only Wi-Fi test credentials instead of resending a password.</summary>
    public bool UseTestedWifi { get; set; }
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
                json.TryGetProperty("tcp_cli_enabled", out var tcp) &&
                    tcp.ValueKind is not (JsonValueKind.True or JsonValueKind.False) ||
                json.TryGetProperty("tcp_cli_auth_required", out var tcpAuth) &&
                    tcpAuth.ValueKind is not (JsonValueKind.True or JsonValueKind.False) ||
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
    public const int WirelessFeatureSize = 54;
    public const byte WirelessCommandReportId = 34;
    public const int WirelessCommandFeatureSize = 102;
    public const byte WirelessResultReportId = 35;
    public const int WirelessResultFeatureSize = 223;

    public static byte[] BuildWirelessRequest(WirelessRequest settings, byte transaction,
        bool tcpCliConfigurable = true, bool tcpCliAuthConfigurable = true)
    {
        if (!tcpCliConfigurable && settings.TcpCliEnabled is not null)
            throw new ArgumentException("TCP CLI setting requires wireless status v2");
        if (!tcpCliAuthConfigurable && settings.TcpCliAuthRequired is not null)
            throw new ArgumentException("TCP CLI authentication setting requires wireless status v3");
        if (!settings.ConfirmReboot || transaction == 0 || settings.WifiEnabled is null ||
            settings.BleEnabled is null || settings.Ssid is null || settings.Extra?.Count > 0 ||
            settings.Board is not { SystemClockMhz: 0 or 150 or 200 or 250, SdCardProfile: 0 or 1, Radios: >= 0 and <= 3 })
            throw new ArgumentException("Invalid wireless configuration or missing reboot confirmation");
        if (settings.WifiEnabled.Value != ((settings.Board.Radios & 1) != 0) ||
            settings.BleEnabled.Value != ((settings.Board.Radios & 2) != 0))
            throw new ArgumentException("Wireless enable flags must match the board radio switches");
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
        if (tcpCliConfigurable)
            report[4] = settings.TcpCliEnabled is null ? (byte)16 : settings.TcpCliEnabled.Value ? (byte)0 : (byte)8;
        if (tcpCliAuthConfigurable)
            report[4] |= settings.TcpCliAuthRequired is null
                ? settings.BleKey == "" ? (byte)32 : (byte)64
                : settings.TcpCliAuthRequired.Value ? (byte)32 : (byte)0;
        if (settings.UseTestedWifi) report[4] |= 4;
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(134), (ushort)settings.Board.SystemClockMhz);
        report[136] = (byte)settings.Board.SdCardProfile;
        ssid.CopyTo(report, 5);
        if (settings.Password is null && !settings.UseTestedWifi) report[4] |= 1;
        else if (settings.UseTestedWifi && settings.Password is not null)
            throw new ArgumentException("Do not resend a password after a successful test join");
        else if (!settings.UseTestedWifi)
        {
            var password = settings.Password ?? throw new ArgumentException("Invalid Wi-Fi password");
            if (password.Length != 0 && (password.Length is < 8 or > 63 ||
                password.Any(c => c is < ' ' or > '~')))
                throw new ArgumentException("Password must be empty or contain 8..63 printable ASCII characters");
            Encoding.ASCII.GetBytes(password).CopyTo(report, 38);
        }
        if (settings.BleKey is null) report[4] |= 2;
        else if (settings.BleKey.Length != 0) // an empty key erases the saved key; the report field stays zero
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

    public static byte[] BuildWirelessCommand(byte transaction, byte operation, byte page = 0,
        string? ssid = null, string? password = null)
    {
        if (transaction == 0 || operation is < 1 or > 4)
            throw new ArgumentException("Invalid wireless command");
        var report = new byte[WirelessCommandFeatureSize];
        report[0] = WirelessCommandReportId;
        report[1] = 1;
        report[2] = operation;
        report[3] = transaction;
        if (operation == 2)
        {
            if (page > 2) throw new ArgumentOutOfRangeException(nameof(page));
            report[4] = page;
        }
        else if (operation == 3)
        {
            if (ssid is null) throw new ArgumentException("SSID is required for a connection test");
            byte[] encoded;
            try { encoded = new UTF8Encoding(false, true).GetBytes(ssid); }
            catch (EncoderFallbackException) { throw new ArgumentException("Invalid SSID encoding"); }
            password ??= "";
            if (encoded.Length is 0 or > 32 || encoded.Contains((byte)0))
                throw new ArgumentException("SSID must contain 1..32 UTF-8 bytes");
            if (password.Length != 0 && (password.Length is < 8 or > 63 ||
                password.Any(c => c is < ' ' or > '~')))
                throw new ArgumentException("Password must contain 8..63 printable ASCII characters");
            report[5] = (byte)encoded.Length;
            report[6] = (byte)password.Length;
            encoded.CopyTo(report, 7);
            Encoding.ASCII.GetBytes(password).CopyTo(report, 39);
        }
        return report;
    }

    public static WirelessOperationResponse ParseWirelessOperation(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, WirelessResultReportId, 222);
        var maxPage = payload[2] >= 4 ? payload[0] == 1 ? 8 : WirelessOperationResponse.Details.Length - 1 : 2;
        Require(payload[0] is 1 or 2 && payload[2] <= 6 && payload[3] <= maxPage && payload[4] <= 18 && payload[5] <= 3,
            "Invalid wireless scan/test result");
        var networks = new List<WirelessNetworkResponse>();
        for (var index = 0; index < 6; index++)
        {
            var entry = payload.Slice(6 + index * 35, 35);
            var length = entry[0];
            if (length == 0)
            {
                Require(entry.IndexOfAnyExcept((byte)0) < 0, "Invalid empty wireless scan entry");
                continue;
            }
            Require(length <= 32 && entry.Slice(1 + length, 32 - length).IndexOfAnyExcept((byte)0) < 0
                && entry[34] <= (payload[0] == 1 ? 1 : 7), "Invalid wireless scan entry");
            string ssid;
            try { ssid = new UTF8Encoding(false, true).GetString(entry.Slice(1, length)); }
            catch (DecoderFallbackException) { throw new InvalidOperationException("Invalid wireless scan SSID"); }
            var security = payload[0] == 1 ? entry[34] != 0 ? "Secured" : "Open"
                : new[] { "Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)", "RSN (WPA2/WPA3)",
                    "WPA/RSN", "WPA/RSN" }[entry[34]];
            networks.Add(new(ssid, unchecked((sbyte)entry[33]), entry[34] != 0, security));
        }
        var remainingSeconds = BinaryPrimitives.ReadUInt16LittleEndian(payload[220..]);
        var staged = payload[2] == 5 || payload[2] == 6 && payload[3] == 9;
        Require(networks.Count <= payload[4] && (payload[2] == 5 ||
            payload[216..220].IndexOfAnyExcept((byte)0) < 0) && remainingSeconds <= 600 &&
            (staged || remainingSeconds == 0),
            "Invalid wireless scan/test result fields");
        var ip = string.Join(".", payload[216..220].ToArray());
        var detail = payload[2] >= 4 ? payload[3] : (byte)0;
        return new(payload[1], payload[2], payload[2] >= 4 ? (byte)0 : payload[3], payload[4], payload[5], ip, networks,
            detail, staged ? remainingSeconds : (ushort)0);
    }

    public static WirelessStatusResponse ParseWirelessStatus(ReadOnlySpan<byte> report)
    {
        var offset = report.Length >= 2 && (report[0] == 0 || report[0] == WirelessReportId)
                && report[1] == WirelessReportId ? 2
            : report.Length != 0 && report[0] == WirelessReportId ? 1 : -1;
        Require(offset >= 0 && report.Length > offset, "Invalid wireless status report");
        var payloadSize = report[offset] >= 4 ? 53 : 52;
        var payload = FeaturePayload(report, WirelessReportId, payloadSize, report[offset]);
        var restartFlag = payloadSize == 53 ? payload[52] : (byte)0;
        Require(payload[0] is 1 or 2 or 3 or 4 && (payload[1] & ~3) == 0 && payload[2] <= 6 &&
            payload[4] <= 3 && payload[42] <= 1 && payload[43] <= 1 && payload[44] <= 3 &&
            payload[45] <= 2 && payload[46] <= 1 &&
            payload[47] <= (payload[0] == 1 ? 0 : payload[0] == 2 ? 1 : 3) &&
            (payload[0] >= 3 ? payload[48] <= 3 : payload[48..52].IndexOfAnyExcept((byte)0) < 0) &&
            restartFlag <= 1,
            "Invalid wireless status report");
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
            $"{payload[5]}.{payload[6]}.{payload[7]}.{payload[8]}", payload[44],
            new[] { "off", "advertising", "connected" }[payload[45]], payload[46] != 0,
            (payload[47] & 1) == 0, payload[0] >= 2, (payload[47] & 2) != 0, payload[0] >= 3,
            payload[0] < 3 ? null : new[] { "try_wpa2", "try_sae", "wpa2", "sae" }[payload[48]],
            payload[0] < 3 ? (ushort)0 : BinaryPrimitives.ReadUInt16LittleEndian(payload[49..]),
            payload[0] < 3 || payload[51] == 127 ? null : unchecked((sbyte)payload[51]),
            payload[0] >= 4 && restartFlag != 0);
    }
}
