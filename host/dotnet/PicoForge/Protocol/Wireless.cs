using System.Buffers;
using System.Buffers.Binary;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PicoForge;

/// <summary>Report 12 runtime state and Report 2 saved switches; never contains credentials.</summary>
public sealed record WirelessStatusResponse(bool WifiEnabled, bool BleEnabled, string Ssid,
    string WifiState, string Ipv4, byte Error, string BleState, bool TcpCliStarted, bool TcpCliEnabled,
    bool NetworkConfigured, bool BleKeyConfigured, string SsidHex, bool SsidStaged, bool PasswordStaged,
    bool KeyStaged, bool PairTested, byte StagedState, byte ScanState, ushort ScanGeneration, byte ScanCount,
    byte TestGeneration)
{
    public bool TcpCliAuthRequired => BleKeyConfigured;
    public string Phase => HidProtocol.WifiDetail(Error);
}

public sealed record WirelessResponse(WirelessStatusResponse Wireless, bool RebootPending,
    BootSettingsResponse? Settings = null);

/// <summary>Opaque SSID bytes and their escaped display label.</summary>
public sealed record WirelessNetworkResponse(string Ssid, int Rssi, bool Secured, string Security,
    string SsidHex);

/// <summary>Retained test result associated with a Report 12 test generation.</summary>
public sealed record WirelessOperationResponse(byte State, byte WifiState, string Ipv4, byte Detail,
    byte TestGeneration, ushort RemainingSeconds = 600)
{
    public string Phase => HidProtocol.WifiDetail(Detail);
}

/// <summary>Confirmed atomic save; omitted credentials are not staged.</summary>
public sealed class WirelessRequest
{
    public bool? WifiEnabled { get; set; }
    public bool? BleEnabled { get; set; }
    public bool? TcpCliEnabled { get; set; }
    public string? Ssid { get; set; }
    public string? SsidHex { get; set; }
    public string? Password { get; set; }
    public string? BleKey { get; set; }
    public BootSettingsResponse? Board { get; set; }
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
                json.TryGetProperty("ssid", out var ssid) && ssid.ValueKind != JsonValueKind.String ||
                json.TryGetProperty("ssid_hex", out var hex) && hex.ValueKind != JsonValueKind.String ||
                json.TryGetProperty("tcp_cli_enabled", out var tcp) &&
                    tcp.ValueKind is not (JsonValueKind.True or JsonValueKind.False) ||
                !json.TryGetProperty("board", out var board) || board.ValueKind != JsonValueKind.Object ||
                !board.TryGetProperty("system_clock_mhz", out _) || !board.TryGetProperty("sdcard_profile", out _) ||
                !board.TryGetProperty("radios", out _))
                throw new JsonException();
            HidProtocol.ValidateWirelessRequest(request);
            return request;
        }
        catch (Exception error) when (error is JsonException or ArgumentException)
        {
            throw new ArgumentException(
                "Invalid wireless configuration; confirm reboot and check field types and lengths");
        }
    }
}

internal static partial class HidProtocol
{
    public const byte WirelessReportId = 12;
    public const int WirelessFeatureSize = 49;

    public static void ValidateWirelessRequest(WirelessRequest settings)
    {
        if (!settings.ConfirmReboot || settings.WifiEnabled is null || settings.BleEnabled is null ||
            settings.Extra?.Count > 0 ||
            settings.Board is not
                { SystemClockMhz: 0 or 150 or 200 or 250, SdCardProfile: 0 or 1, Radios: >= 0 and <= 3 })
            throw new ArgumentException("Invalid wireless configuration or missing reboot confirmation");
        if (settings.WifiEnabled.Value != ((settings.Board.Radios & 1) != 0) ||
            settings.BleEnabled.Value != ((settings.Board.Radios & 2) != 0))
            throw new ArgumentException("Wireless enable flags must match the board radio switches");
        if (settings.Ssid is not null || settings.SsidHex is not null)
            EncodeSsid(settings.Ssid, settings.SsidHex);
        if (settings.Password is not null) ValidatePassword(settings.Password);
        if (settings.BleKey is not null) EncodeControlKey(settings.BleKey);
        if (settings.UseTestedWifi && settings.Password is not null)
            throw new ArgumentException("Do not resend a password after a successful test join");
    }

    public static byte[] EncodeSsid(string? ssid, string? hex = null)
    {
        byte[] bytes;
        if (hex is not null)
        {
            if (hex.Length > 64 || hex.Length % 2 != 0 || !hex.All(Uri.IsHexDigit))
                throw new ArgumentException("Invalid SSID hex");
            bytes = Convert.FromHexString(hex);
        }
        else
        {
            if (ssid is null) throw new ArgumentException("SSID is required");
            try { bytes = new UTF8Encoding(false, true).GetBytes(ssid); }
            catch (EncoderFallbackException) { throw new ArgumentException("Invalid SSID encoding"); }
        }
        if (bytes.Length > 32) throw new ArgumentException("SSID must contain at most 32 bytes");
        return bytes;
    }

    public static void ValidatePassword(string password)
    {
        if (password.Length != 0 && (password.Length is < 8 or > 63 || password.Any(c => c is < ' ' or > '~')))
            throw new ArgumentException("Password must contain 8..63 printable ASCII characters");
    }

    public static byte[] EncodeControlKey(string key)
    {
        if (key.Length == 0) return [];
        if (key.Length != 64 || !key.All(Uri.IsHexDigit)) throw new ArgumentException("Control key must be 32 bytes");
        return Convert.FromHexString(key);
    }

    public static byte[] StagingArguments(byte opcode, ReadOnlySpan<byte> value)
    {
        if (opcode is not (6 or 7 or 8) || value.Length > (opcode == 7 ? 63 : 32))
            throw new ArgumentException("Invalid staging arguments");
        var arguments = new byte[64];
        arguments[0] = (byte)value.Length;
        value.CopyTo(arguments.AsSpan(1));
        return arguments;
    }

    public static WirelessStatusResponse ParseWirelessStatus(ReadOnlySpan<byte> report,
        BoardConfigurationResponse configuration)
    {
        var payload = FeaturePayload(report, WirelessReportId, 48);
        Require(payload[0] == ReportVersion && payload[1] <= 6 && payload[3] <= 2 &&
            payload[9] <= 32 && payload.Slice(10 + payload[9], 32 - payload[9]).IndexOfAnyExcept((byte)0) < 0 &&
            payload[42] <= 3 && payload[45] <= 18 && (payload[47] & 0xf0) == 0,
            "Invalid wireless status report");
        var flags = payload[4];
        var staged = payload[47];
        var scanGeneration = BinaryPrimitives.ReadUInt16LittleEndian(payload[43..]);
        Require((payload[42] == 2 || payload[45] == 0) &&
            (payload[42] == 0 ? scanGeneration == 0 : scanGeneration != 0) &&
            ((staged & 8) == 0 || (flags & 0xf0) == 0) &&
            ((staged & 1) == 0 || (flags & 0x10) != 0) &&
            ((staged & 2) == 0 || (flags & 0x20) != 0) &&
            ((staged & 4) == 0 || (flags & 0x40) != 0) &&
            ((staged & 1) == 0 || (staged & 2) != 0 && (flags & 0x30) == 0x30) &&
            ((flags & 0x80) == 0 || (flags & 0x30) != 0 && payload[46] != 0) &&
            ((flags & 4) == 0 || (flags & 2) != 0) &&
            (payload[3] == 0 || (flags & 2) != 0) &&
            (payload[1] < 4 || payload[46] != 0), "Invalid wireless status fields");
        var ssid = payload.Slice(10, payload[9]).ToArray();
        return new((configuration.BoardFlags & 8) != 0, (configuration.BoardFlags & 16) != 0, DisplaySsid(ssid),
            new[] { "disabled", "connecting", "connected", "failed", "testing", "test_joined",
                "test_failed" }[payload[1]],
            string.Join(".", payload[5..9].ToArray()), payload[2] <= 6 ? payload[2] : (byte)0,
            new[] { "off", "advertising", "connected" }[payload[3]], (flags & 4) != 0,
            (configuration.BoardFlags & 32) != 0, (flags & 1) != 0, (flags & 2) != 0,
            Convert.ToHexString(ssid), (flags & 16) != 0, (flags & 32) != 0,
            (flags & 64) != 0, (flags & 128) != 0, staged, payload[42], scanGeneration, payload[45], payload[46]);
    }

    public static WirelessNetworkResponse ParseScanEntry(ControlResult result, ushort generation, byte index)
    {
        var data = result.ResultData;
        Require(result.Opcode == 12 && result.Status == 2 &&
            BinaryPrimitives.ReadUInt16LittleEndian(data) == generation && data[2] == index,
            "Scan entry belongs to another scan");
        var ssid = data.AsSpan(4, data[3]).ToArray();
        var security = new[] { "Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)", "RSN (WPA2/WPA3)",
            "WPA/RSN", "WPA/RSN" }[data[37]];
        return new(DisplaySsid(ssid), unchecked((sbyte)data[36]), data[37] != 0, security,
            Convert.ToHexString(ssid));
    }

    public static string WifiDetail(byte detail) => detail switch
    {
        1 => "Radio initialization failed", 2 => "Network not found", 3 => "Password rejected",
        4 => "Connected but no IP address from DHCP", 5 => "Timed out", 6 => "Test connection lost", _ => ""
    };

    public static string DisplaySsid(ReadOnlySpan<byte> bytes)
    {
        var display = new StringBuilder();
        while (!bytes.IsEmpty)
        {
            var status = Rune.DecodeFromUtf8(bytes, out var rune, out var consumed);
            if (status != OperationStatus.Done || Rune.GetUnicodeCategory(rune) is
                UnicodeCategory.Control or UnicodeCategory.Format or UnicodeCategory.LineSeparator or
                UnicodeCategory.ParagraphSeparator)
            {
                var length = status == OperationStatus.Done ? consumed : 1;
                foreach (var value in bytes[..length]) display.Append($"\\x{value:X2}");
                bytes = bytes[length..];
            }
            else
            {
                display.Append(rune.ToString());
                bytes = bytes[consumed..];
            }
        }
        return display.ToString();
    }
}
