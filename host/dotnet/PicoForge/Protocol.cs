using System.Text;

namespace PicoForge;

/// <summary>Hardware and product identity reported by the PicoForge HID feature report.</summary>
public sealed record HardwareInfoResponse(string ProductName, string Mcu, byte McuId,
    string SerialNumber);

/// <summary>Dynamic telemetry reported periodically by the PicoForge HID input report.</summary>
public sealed record RuntimeStatusResponse(byte Sequence, uint UptimeMs, uint SystemClockHz,
    bool UsbMounted, bool CdcOpen, bool TemperatureValid, short? TemperatureCentidegrees,
    bool LedAvailable, bool LedOn)
{
    public BoardStatusResponse ToBoardStatus() => new(TemperatureValid, TemperatureCentidegrees,
        LedAvailable, LedOn);
}

/// <summary>Board temperature and onboard LED state derived from dynamic HID input telemetry.</summary>
public sealed record BoardStatusResponse(bool TemperatureValid, short? TemperatureCentidegrees,
    bool LedAvailable, bool LedOn);

/// <summary>Current HID connection and the most recent hardware/runtime reports.</summary>
public sealed record StatusResponse(bool Connected, string? Error, HardwareInfoResponse? Hardware,
    RuntimeStatusResponse? Runtime, DateTimeOffset? UpdatedAt, BoardStatusResponse? BoardStatus);

internal static class HidProtocol
{
    public const int VendorId = 0xCAFE;
    public const int ProductId = 0x4011;
    public const uint Usage = 0xFF000001;
    public const byte InfoReportId = 1;
    public const byte StatusReportId = 2;
    public const byte LedCommandReportId = 4;
    public const byte ReportVersion = 2;
    public const int InfoPayloadSize = 42;
    public const int StatusPayloadSize = 13;
    public const int InfoFeatureSize = InfoPayloadSize + 1;
    public const int StatusInputSize = StatusPayloadSize + 1;

    public static HardwareInfoResponse ParseInfoReport(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, InfoReportId, InfoPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID info report version");
        var mcuId = payload[1];
        var productBytes = payload.Slice(2, 20);
        var terminator = productBytes.IndexOf((byte)0);
        if (terminator >= 0) productBytes = productBytes[..terminator];
        Require(!productBytes.IsEmpty && productBytes.IndexOfAnyInRange((byte)0x80, byte.MaxValue) < 0,
            "invalid product name in HID info report");
        var serialBytes = payload.Slice(22, 16);
        var serialNumber = Encoding.ASCII.GetString(serialBytes);
        Require(serialNumber.Length == 16 && serialNumber.All(Uri.IsHexDigit),
            "invalid USB serial in HID info report");
        Require(payload.Slice(38, 4).IndexOfAnyExcept((byte)0) < 0,
            "nonzero reserved bytes in HID info report");
        var mcu = mcuId switch
        {
            1 => "RP2040",
            2 => "RP2350",
            _ => $"Unknown MCU ({mcuId})"
        };
        return new(Encoding.ASCII.GetString(productBytes), mcu, mcuId, serialNumber);
    }

    public static RuntimeStatusResponse ParseStatusReport(ReadOnlySpan<byte> report)
    {
        Require(report.Length >= StatusInputSize && report[0] == StatusReportId,
            "unexpected HID status report length or ID");
        Require(report[StatusInputSize..].IndexOfAnyExcept((byte)0) < 0,
            "unexpected HID status report padding");
        var payload = report.Slice(1, StatusPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID status report version");
        var flags = payload[10];
        Require((flags & ~0x1f) == 0, "unknown HID status flags");
        var clockHz = ReadUInt32LittleEndian(payload[6..]);
        Require(clockHz != 0, "invalid system clock in HID status report");
        var temperatureValid = (flags & 0x04) != 0;
        var ledAvailable = (flags & 0x08) != 0;
        var ledOn = (flags & 0x10) != 0;
        Require(!ledOn || ledAvailable, "LED state reported without an available LED");
        var temperature = (short)(payload[11] | payload[12] << 8);
        return new(payload[1], ReadUInt32LittleEndian(payload[2..]), clockHz,
            (flags & 0x01) != 0, (flags & 0x02) != 0,
            temperatureValid, temperatureValid ? temperature : null, ledAvailable, ledOn);
    }

    public static byte[] BuildLedCommandReport(bool on) =>
        [LedCommandReportId, ReportVersion, on ? (byte)1 : (byte)0];

    private static ReadOnlySpan<byte> FeaturePayload(ReadOnlySpan<byte> report, byte reportId, int size)
    {
        var offset = report.Length >= size + 2 && report[0] == 0 && report[1] == reportId ? 2
            : report.Length >= size + 2 && report[0] == reportId && report[1] == reportId
                && report[2] == ReportVersion ? 2
            : report.Length >= size + 1 && report[0] == reportId ? 1
            : -1;
        Require(offset >= 0 && report.Length >= offset + size, "unexpected HID feature report length or ID");
        Require(report[(offset + size)..].IndexOfAnyExcept((byte)0) < 0, "unexpected HID feature report padding");
        return report.Slice(offset, size);
    }

    private static uint ReadUInt32LittleEndian(ReadOnlySpan<byte> source) =>
        (uint)(source[0] | source[1] << 8 | source[2] << 16 | source[3] << 24);

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}