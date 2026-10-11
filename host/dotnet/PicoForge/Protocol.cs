using System.Buffers.Binary;
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

/// <summary>Card identity decoded from the SD CID register.</summary>
public sealed record SdCardIdentity(byte ManufacturerId, string OemId, string ProductName,
    string ProductRevision, uint SerialNumber, int ManufactureYear, int ManufactureMonth);

/// <summary>Presence, transport, negotiated bus, capacity, and card identity.</summary>
public sealed record MemoryStatusResponse(byte Sequence, bool Configured, bool PresenceKnown,
    bool? Present, bool Initialized, string? ComponentType, string? Mode, uint? BusClockHz,
    uint? BlockCount, ulong? CapacityBytes, SdCardIdentity? Manufacturer);

internal sealed record InputReport(RuntimeStatusResponse? Runtime, MemoryStatusResponse? Memory);

/// <summary>Current HID connection and the most recent hardware/runtime reports.</summary>
public sealed record StatusResponse(bool Connected, string? Error, HardwareInfoResponse? Hardware,
    RuntimeStatusResponse? Runtime, DateTimeOffset? UpdatedAt, BoardStatusResponse? BoardStatus,
    MemoryStatusResponse? Memory);

internal static class HidProtocol
{
    public const int VendorId = 0xCAFE;
    public const int ProductId = 0x4011;
    public const uint Usage = 0xFF000001;
    public const byte InfoReportId = 1;
    public const byte StatusReportId = 2;
    public const byte MemoryReportId = 3;
    public const byte LedCommandReportId = 4;
    public const byte ReportVersion = 2;
    public const int InfoPayloadSize = 42;
    public const int StatusPayloadSize = 13;
    public const int MemoryPayloadSize = 29;
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

    public static MemoryStatusResponse ParseMemoryReport(ReadOnlySpan<byte> report)
    {
        Require(report.Length >= MemoryPayloadSize + 1 && report[0] == MemoryReportId,
            "unexpected HID memory report length or ID");
        Require(report[(MemoryPayloadSize + 1)..].IndexOfAnyExcept((byte)0) < 0,
            "unexpected HID memory report padding");
        var payload = report[1..];
        Require(payload[0] == ReportVersion, "unsupported HID memory report version");
        var flags = payload[2];
        Require((flags & ~0x3f) == 0, "unknown HID memory flags");
        var component = payload[3];
        var mode = payload[4];
        Require(component <= 1 && mode <= 2, "unknown HID memory type or mode");
        var configured = (flags & 0x01) != 0;
        var presenceKnown = (flags & 0x02) != 0;
        var present = (flags & 0x04) != 0;
        var initialized = (flags & 0x08) != 0;
        var cidValid = (flags & 0x10) != 0;
        var capacityValid = (flags & 0x20) != 0;
        Require(!present || presenceKnown, "memory reported present without known presence");
        Require(!initialized || (configured && presenceKnown && present),
            "initialized memory requires a configured, present card");
        Require(cidValid == initialized && capacityValid == initialized,
            "initialized memory metadata flags disagree");
        var busClockHz = ReadUInt32LittleEndian(payload[5..]);
        var blockCount = ReadUInt32LittleEndian(payload[9..]);
        var cid = payload.Slice(13, 16);
        Require(initialized == (busClockHz != 0 && blockCount != 0) &&
                (initialized || cid.IndexOfAnyExcept((byte)0) < 0),
            "invalid memory metadata values");
        SdCardIdentity? identity = null;
        if (cidValid)
        {
            var oem = Encoding.ASCII.GetString(cid.Slice(1, 2));
            var product = Encoding.ASCII.GetString(cid.Slice(3, 5));
            Require(oem.All(character => character is >= ' ' and <= '~') &&
                    product.All(character => character is >= ' ' and <= '~'), "invalid text in SD CID");
            var revision = cid[8];
            var date = ((cid[13] & 0x0f) << 8) | cid[14];
            identity = new(cid[0], oem, product, $"{revision >> 4}.{revision & 0x0f}",
                BinaryPrimitives.ReadUInt32BigEndian(cid[9..]), 2000 + (date >> 4), date & 0x0f);
        }
        return new(payload[1], configured, presenceKnown, presenceKnown ? present : null,
            initialized, component == 1 ? "sd-card" : null, mode switch
            {
                1 => "sdio",
                2 => "spi",
                _ => null
            }, initialized ? busClockHz : null, capacityValid ? blockCount : null,
            capacityValid ? (ulong)blockCount * 512 : null, identity);
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