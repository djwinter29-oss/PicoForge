// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

using System.Buffers;
using System.Buffers.Binary;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PicoForge;

/// <summary>Wi-Fi states in Feature 12 and the shared dashboard JSON contract.</summary>
public enum WifiState
{
    /// <summary>Wi-Fi is disabled.</summary>
    Disabled,
    /// <summary>Joining the saved network.</summary>
    Connecting,
    /// <summary>Connected and assigned an address.</summary>
    Connected,
    /// <summary>Join failed or the link was lost.</summary>
    Failed,
    /// <summary>A staged pair is being tested.</summary>
    Testing,
    /// <summary>The staged pair passed its connection test.</summary>
    TestJoined,
    /// <summary>The staged pair failed its connection test.</summary>
    TestFailed
}

/// <summary>Bluetooth states reported in Feature 12.</summary>
public enum BleState
{
    /// <summary>Bluetooth is off.</summary>
    Off,
    /// <summary>The board is advertising.</summary>
    Advertising,
    /// <summary>A Bluetooth connection is active.</summary>
    Connected
}

internal enum WirelessScanState : byte
{
    Idle = 0,
    Scanning = 1,
    Complete = 2,
    Failed = 3
}

[Flags]
internal enum WirelessStatusFlags : byte
{
    NetworkConfigured = 1 << 0,
    BleKeyConfigured = 1 << 1,
    TcpCliStarted = 1 << 2,
    SsidStaged = 1 << 4,
    PasswordStaged = 1 << 5,
    KeyStaged = 1 << 6,
    PairTested = 1 << 7
}

[Flags]
internal enum WirelessStagingFlags : byte
{
    Ssid = 1 << 0,
    Password = 1 << 1,
    Key = 1 << 2,
    PairTested = 1 << 3
}

internal static class DashboardJson
{
    public static JsonSerializerOptions Options { get; } = CreateOptions();

    public static void Configure(JsonSerializerOptions options)
    {
        options.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower;
        options.PropertyNameCaseInsensitive = false;
        options.RespectRequiredConstructorParameters = true;
        options.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.SnakeCaseLower));
    }

    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions();
        Configure(options);
        return options;
    }
}

/// <summary>Report 12 runtime state and Report 2 saved switches; never contains credentials.</summary>
public sealed record WirelessStatusResponse
{
    public bool WifiEnabled { get; init; }
    public bool BleEnabled { get; init; }
    public string Ssid { get; init; } = "";
    public WifiState WifiState { get; init; }
    public string Ipv4 { get; init; } = "";
    public byte Error { get; init; }
    public BleState BleState { get; init; }
    public bool TcpCliStarted { get; init; }
    public bool TcpCliEnabled { get; init; }
    public bool NetworkConfigured { get; init; }
    public bool BleKeyConfigured { get; init; }
    public string SsidHex { get; init; } = "";
    public bool SsidStaged { get; init; }
    public bool PasswordStaged { get; init; }
    public bool KeyStaged { get; init; }
    public bool PairTested { get; init; }
    public byte StagedState { get; init; }
    public byte ScanState { get; init; }
    public ushort ScanGeneration { get; init; }
    public byte ScanCount { get; init; }
    public byte TestGeneration { get; init; }

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
            var request = json.Deserialize<WirelessRequest>(DashboardJson.Options)
                ?? throw new JsonException();
            if (HasExplicitNull(json, "password") || HasExplicitNull(json, "ble_key") ||
                HasExplicitNull(json, "ssid") || HasExplicitNull(json, "ssid_hex") ||
                HasExplicitNull(json, "tcp_cli_enabled"))
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

    private static bool HasExplicitNull(JsonElement json, string name) =>
        json.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Null;
}

internal static partial class HidProtocol
{
    public const byte WirelessReportId = 12;
    public const int WirelessFeatureSize = 49;

    public static void ValidateWirelessRequest(WirelessRequest settings)
    {
        if (!settings.ConfirmReboot || settings.WifiEnabled is null || settings.BleEnabled is null ||
            settings.Extra?.Count > 0 || settings.Board is null)
            throw new ArgumentException("Invalid wireless configuration or missing reboot confirmation");
        ValidateSettings(settings.Board);
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

    public static byte[] StagingArguments(Opcode opcode, ReadOnlySpan<byte> value)
    {
        if (opcode is not (Opcode.StageSsid or Opcode.StagePassword or Opcode.StageKey) ||
            value.Length > (opcode == Opcode.StagePassword ? 63 : 32))
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
        Require(payload[0] == ReportVersion && payload[1] <= (byte)WifiState.TestFailed &&
            payload[3] <= (byte)BleState.Connected &&
            payload[9] <= 32 && payload.Slice(10 + payload[9], 32 - payload[9]).IndexOfAnyExcept((byte)0) < 0 &&
            payload[42] <= (byte)WirelessScanState.Failed && payload[45] <= 18 &&
            (payload[47] & 0xf0) == 0,
            "Invalid wireless status report");
        var flags = (WirelessStatusFlags)payload[4];
        var staged = (WirelessStagingFlags)payload[47];
        var scanGeneration = BinaryPrimitives.ReadUInt16LittleEndian(payload[43..]);
        Require(((WirelessScanState)payload[42] == WirelessScanState.Complete || payload[45] == 0) &&
            ((WirelessScanState)payload[42] == WirelessScanState.Idle
                ? scanGeneration == 0 : scanGeneration != 0) &&
            ((staged & WirelessStagingFlags.PairTested) == 0 ||
                (flags & (WirelessStatusFlags.SsidStaged | WirelessStatusFlags.PasswordStaged |
                    WirelessStatusFlags.KeyStaged | WirelessStatusFlags.PairTested)) == 0) &&
            ((staged & WirelessStagingFlags.Ssid) == 0 || (flags & WirelessStatusFlags.SsidStaged) != 0) &&
            ((staged & WirelessStagingFlags.Password) == 0 ||
                (flags & WirelessStatusFlags.PasswordStaged) != 0) &&
            ((staged & WirelessStagingFlags.Key) == 0 || (flags & WirelessStatusFlags.KeyStaged) != 0) &&
            ((staged & WirelessStagingFlags.Ssid) == 0 ||
                (staged & WirelessStagingFlags.Password) != 0 &&
                (flags & (WirelessStatusFlags.SsidStaged | WirelessStatusFlags.PasswordStaged)) ==
                    (WirelessStatusFlags.SsidStaged | WirelessStatusFlags.PasswordStaged)) &&
            ((flags & WirelessStatusFlags.PairTested) == 0 ||
                (flags & (WirelessStatusFlags.SsidStaged | WirelessStatusFlags.PasswordStaged)) != 0 &&
                payload[46] != 0) &&
            ((flags & WirelessStatusFlags.TcpCliStarted) == 0 ||
                (flags & WirelessStatusFlags.BleKeyConfigured) != 0) &&
            (payload[3] == (byte)BleState.Off ||
                (flags & WirelessStatusFlags.BleKeyConfigured) != 0) &&
            (payload[1] < (byte)WifiState.Testing || payload[46] != 0), "Invalid wireless status fields");
        var ssid = payload.Slice(10, payload[9]).ToArray();
        var saved = configuration.BoardFlags;
        return new()
        {
            WifiEnabled = (saved & BoardFlagWifi) != 0,
            BleEnabled = (saved & BoardFlagBle) != 0,
            Ssid = DisplaySsid(ssid),
            WifiState = (WifiState)payload[1],
            Ipv4 = string.Join(".", payload[5..9].ToArray()),
            Error = payload[2] <= 6 ? payload[2] : (byte)0,
            BleState = (BleState)payload[3],
            TcpCliStarted = (flags & WirelessStatusFlags.TcpCliStarted) != 0,
            TcpCliEnabled = (saved & BoardFlagTcpCli) != 0,
            NetworkConfigured = (flags & WirelessStatusFlags.NetworkConfigured) != 0,
            BleKeyConfigured = (flags & WirelessStatusFlags.BleKeyConfigured) != 0,
            SsidHex = Convert.ToHexString(ssid),
            SsidStaged = (flags & WirelessStatusFlags.SsidStaged) != 0,
            PasswordStaged = (flags & WirelessStatusFlags.PasswordStaged) != 0,
            KeyStaged = (flags & WirelessStatusFlags.KeyStaged) != 0,
            PairTested = (flags & WirelessStatusFlags.PairTested) != 0,
            StagedState = payload[47],
            ScanState = payload[42],
            ScanGeneration = scanGeneration,
            ScanCount = payload[45],
            TestGeneration = payload[46]
        };
    }

    public static WirelessNetworkResponse ParseScanEntry(ControlResult result, ushort generation, byte index)
    {
        var data = result.ResultData;
        Require(result.Opcode == Opcode.WifiEntry && result.Status == ControlStatus.Completed &&
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
