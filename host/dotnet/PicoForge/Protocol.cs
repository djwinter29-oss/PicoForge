using System.Buffers.Binary;
using System.Text;
using System.Text.Json.Serialization;

namespace PicoForge;

/// <summary>Device identity and capabilities from HID Feature report 1.</summary>
public sealed record HardwareInfoResponse(string ProductName, string Mcu, byte McuId,
    string SerialNumber, string? FirmwareVersion = null, ushort ProductId = 0,
    byte BoardModel = 0, byte Capabilities = 0, uint RamTotalBytes = 0, uint RamReservedBytes = 0);

/// <summary>Persisted next-boot clock and selected transport speed profile.</summary>
public sealed record BootSettingsResponse(int SystemClockMhz,
    [property: JsonPropertyName("sdcard_profile")] int SdCardProfile);

/// <summary>GPIO topology and persisted configuration from HID Feature report 2.</summary>
public sealed record BoardConfigurationResponse(ushort StoredSystemClockMhz, byte StoredSdCardProfile,
    byte MonitorUartIndex, byte MonitorUartTxGpio, byte MonitorUartRxGpio, byte[] StorageGpios,
    byte BoardFlags, byte ResetCause, byte LedGpio, byte CardDetectGpio);

/// <summary>Dynamic telemetry reported periodically by HID Input report 3.</summary>
public sealed record RuntimeStatusResponse(byte Sequence, uint UptimeMs, uint SystemClockHz,
    bool UsbMounted, bool CdcOpen, bool TemperatureValid, short? TemperatureCentidegrees,
    bool LedAvailable, bool LedOn, ushort? CpuUsageCore0BasisPoints = null,
    ushort? CpuUsageCore1BasisPoints = null, uint? RamUsedBytes = null, uint? RamTotalBytes = null,
    ulong UptimeMilliseconds = 0)
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

/// <summary>SD, filesystem, and active speed state from HID Input report 10.</summary>
public sealed record MemoryStatusResponse(byte Sequence, bool Configured, bool PresenceKnown,
    bool? Present, bool Initialized, string? ComponentType, string? Mode, uint? BusClockHz,
    uint? BlockCount, ulong? CapacityBytes, SdCardIdentity? Manufacturer,
    bool SampleValid = true, byte SpeedMode = 0);

public sealed record MscStatusResponse(bool Enabled, bool TransitionPending, bool RequestRejected,
    bool ForceReturnSupported = true);

/// <summary>Verified SD-card file benchmark progress, elapsed times, and cleanup state.</summary>
public sealed record SelfTestStatusResponse(byte Sequence, string State, uint TargetBytes,
    uint WrittenBytes, uint ReadBytes, uint WriteElapsedMs, uint ReadElapsedMs, uint MismatchCount,
    byte Error, byte FilesystemResult, byte RequestError, bool FileRemains, bool VolumeMounted)
{
    public bool Active => State is "queued" or "mounting" or "writing" or "reading" or "cleanup";
    public double? WriteBytesPerSecond => WriteElapsedMs == 0 ? null : WrittenBytes * 1000.0 / WriteElapsedMs;
    public double? ReadBytesPerSecond => ReadElapsedMs == 0 ? null : ReadBytes * 1000.0 / ReadElapsedMs;
    public double ProgressPercent => TargetBytes == 0 ? 0 : ((double)WrittenBytes + ReadBytes) * 100 / (2.0 * TargetBytes);
}

internal sealed record InputReport(RuntimeStatusResponse? Runtime, MemoryStatusResponse? Memory,
    MscStatusResponse? Msc, SelfTestStatusResponse? SelfTest = null, string? FilesystemFormat = null);

/// <summary>Current HID connection and the most recent hardware/runtime reports.</summary>
public sealed record StatusResponse(bool Connected, string? Error, HardwareInfoResponse? Hardware,
    RuntimeStatusResponse? Runtime, DateTimeOffset? UpdatedAt, BoardStatusResponse? BoardStatus,
    MemoryStatusResponse? Memory, MscStatusResponse? Msc, SelfTestStatusResponse? SelfTest = null,
    string? FilesystemFormat = null, BootSettingsResponse? Settings = null,
    BoardConfigurationResponse? BoardConfiguration = null, DateTimeOffset? TelemetryUpdatedAt = null,
    DateTimeOffset? StorageUpdatedAt = null, DateTimeOffset? SelfTestUpdatedAt = null,
    bool TelemetryStale = true, bool StorageStale = true, bool SelfTestStale = true)
{
    public bool SettingsSupported => Settings is not null;
}

internal sealed record ControlResult(byte TransactionId, byte Opcode, byte Status, byte ErrorReason,
    byte[] ResultData);

internal static class HidProtocol
{
    public const int VendorId = 0xCAFE;
    public const int ProductId = 0x4011;
    public const int ProductIdAlternate = 0x4012;
    public const uint Usage = 0xFF000001;
    public const byte IdentityReportId = 1;
    public const byte ConfigurationReportId = 2;
    public const byte TelemetryReportId = 3;
    public const byte StorageReportId = 10;
    public const byte SelfTestReportId = 11;
    public const byte ControlReportId = 20;
    public const byte ReportVersion = 1;
    public const int IdentityPayloadSize = 26;
    public const int ConfigurationPayloadSize = 17;
    public const int TelemetryPayloadSize = 22;
    public const int StoragePayloadSize = 34;
    public const int SelfTestPayloadSize = 32;
    public const int ControlPayloadSize = 9;
    public const int IdentityFeatureSize = IdentityPayloadSize + 1;
    public const int ConfigurationFeatureSize = ConfigurationPayloadSize + 1;
    public const int ControlFeatureSize = ControlPayloadSize + 1;
    public const byte CapabilityLed = 1 << 0;
    public const byte CapabilityReboot = 1 << 1;
    public const byte CapabilitySettings = 1 << 2;
    public const byte CapabilitySpi = 1 << 3;
    public const byte CapabilitySdio = 1 << 4;
    public const byte CapabilitySelfTest = 1 << 5;

    public static HardwareInfoResponse ParseIdentityReport(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, IdentityReportId, IdentityPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID identity report version");
        var productId = BinaryPrimitives.ReadUInt16LittleEndian(payload[1..]);
        Require(productId == 2, $"unsupported PicoForge product ID 0x{productId:X4}");
        Require(payload[16] == 0, "nonzero reserved identity byte");
        var capabilities = payload[17];
        var spi = (capabilities & CapabilitySpi) != 0;
        var sdio = (capabilities & CapabilitySdio) != 0;
        Require((capabilities & 0xc0) == 0 && spi != sdio, "invalid HID capability flags");
        var mcuId = payload[14];
        var boardModel = payload[15];
        Require(boardModel switch
        {
            1 => mcuId == 1,
            2 or 3 => mcuId == 2,
            _ => true
        }, "board model and MCU do not match");
        var ramTotal = ReadUInt32LittleEndian(payload[18..]);
        var ramReserved = ReadUInt32LittleEndian(payload[22..]);
        Require(ramTotal != 0 && ramReserved <= ramTotal, "invalid firmware RAM reservation");
        var uid = Convert.ToHexString(payload.Slice(3, 8));
        var mcu = mcuId switch
        {
            1 => "RP2040",
            2 => "RP2350",
            _ => $"Unknown MCU ({mcuId})"
        };
        return new("PicoForge", mcu, mcuId, uid,
            $"{payload[11]}.{payload[12]}.{payload[13]}", productId, boardModel,
            capabilities, ramTotal, ramReserved);
    }

    public static HardwareInfoResponse ParseInfoReport(ReadOnlySpan<byte> report) => ParseIdentityReport(report);

    public static BoardConfigurationResponse ParseConfigurationReport(ReadOnlySpan<byte> report,
        HardwareInfoResponse identity)
    {
        var payload = FeaturePayload(report, ConfigurationReportId, ConfigurationPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID board-configuration report version");
        var clock = BinaryPrimitives.ReadUInt16LittleEndian(payload[1..]);
        var sdProfile = payload[3];
        var uartIndex = payload[4];
        var tx = payload[5];
        var rx = payload[6];
        var gpios = payload.Slice(7, 6).ToArray();
        var flags = payload[13];
        var ledGpio = payload[15];
        var detectGpio = payload[16];
        Require(clock is 0 or 150 or 200 or 250, "unsupported persisted system-clock profile");
        Require(sdProfile <= 1, "unsupported persisted SD-card profile");
        Require((flags & ~0x07) == 0, "unknown board-configuration flags");
        Require((uartIndex <= 1 || uartIndex == byte.MaxValue)
            && ((tx == byte.MaxValue) == (rx == byte.MaxValue)), "invalid monitor UART assignment");
        Require(((flags & 0x04) == 0 || (uartIndex <= 1 && tx != byte.MaxValue && rx != byte.MaxValue)),
            "enabled monitor UART requires assigned pins");
        Require(((flags & 0x01) != 0) == (detectGpio != byte.MaxValue)
            && ((flags & 0x02) != 0) == (ledGpio != byte.MaxValue)
            && (((identity.Capabilities & CapabilityLed) != 0) == ((flags & 0x02) != 0)),
            "board flags and GPIO assignments disagree");
        var gpioLimit = identity.McuId == 1 ? 30 : identity.McuId == 2 ? 48 : 256;
        foreach (var pin in gpios.Concat([tx, rx, ledGpio, detectGpio]))
            Require(pin == byte.MaxValue || pin < gpioLimit, "GPIO is invalid for the reported MCU");
        if ((identity.Capabilities & CapabilitySdio) != 0)
            Require(gpios.All(pin => pin != byte.MaxValue), "SDIO requires six configured storage GPIOs");
        else
            Require(gpios.Take(4).All(pin => pin != byte.MaxValue)
                && gpios.Skip(4).All(pin => pin == byte.MaxValue), "invalid SPI storage GPIO layout");
        return new(clock, sdProfile, uartIndex, tx, rx, gpios, flags, payload[14], ledGpio, detectGpio);
    }

    public static BootSettingsResponse ToBootSettings(BoardConfigurationResponse configuration) =>
        new(configuration.StoredSystemClockMhz, configuration.StoredSdCardProfile);

    public static RuntimeStatusResponse ParseTelemetryReport(ReadOnlySpan<byte> report)
    {
        var payload = InputPayload(report, TelemetryReportId, TelemetryPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID telemetry report version");
        var ledFlags = payload[1];
        var temperature = BinaryPrimitives.ReadInt16LittleEndian(payload[2..]);
        var uptime = BinaryPrimitives.ReadUInt64LittleEndian(payload[4..]);
        var core0 = BinaryPrimitives.ReadUInt16LittleEndian(payload[12..]);
        var core1 = BinaryPrimitives.ReadUInt16LittleEndian(payload[14..]);
        var clock = ReadUInt32LittleEndian(payload[16..]);
        var flags = payload[21];
        Require((ledFlags & ~1) == 0 && (flags & ~0x0f) == 0, "unknown HID telemetry flags");
        Require((core0 <= 10000 || core0 == ushort.MaxValue)
            && (core1 <= 10000 || core1 == ushort.MaxValue), "invalid per-core service load");
        Require(clock != 0, "invalid system clock in HID telemetry");
        var temperatureValid = (flags & 0x04) != 0;
        var ledAvailable = (flags & 0x08) != 0;
        var ledOn = (ledFlags & 1) != 0;
        Require(!ledOn || ledAvailable, "LED state reported without an available LED");
        Require(temperatureValid || temperature == 0, "invalid telemetry temperature must be zero");
        return new(payload[20], (uint)uptime, clock, (flags & 1) != 0, (flags & 2) != 0,
            temperatureValid, temperatureValid ? temperature : null, ledAvailable, ledOn,
            core0 == ushort.MaxValue ? null : core0, core1 == ushort.MaxValue ? null : core1,
            UptimeMilliseconds: uptime);
    }

    public static RuntimeStatusResponse ParseStatusReport(ReadOnlySpan<byte> report) => ParseTelemetryReport(report);

    public static InputReport ParseStorageReport(ReadOnlySpan<byte> report, HardwareInfoResponse identity)
    {
        var payload = InputPayload(report, StorageReportId, StoragePayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID storage report version");
        var flags = payload[2];
        var component = payload[3];
        var mode = payload[4];
        var clock = ReadUInt32LittleEndian(payload[5..]);
        var speed = payload[9];
        var blocks = ReadUInt32LittleEndian(payload[10..]);
        var cid = payload.Slice(14, 16);
        var filesystem = payload[30];
        var mscEnabled = payload[31];
        var transition = payload[32];
        var rejected = payload[33];
        var configured = (flags & 0x01) != 0;
        var presenceKnown = (flags & 0x02) != 0;
        var present = (flags & 0x04) != 0;
        var initialized = (flags & 0x08) != 0;
        var cidValid = (flags & 0x10) != 0;
        var capacityValid = (flags & 0x20) != 0;
        var sampleValid = (flags & 0x40) != 0;
        Require((flags & 0x80) == 0 && filesystem <= 4
            && mscEnabled <= 1 && transition <= 1 && rejected <= 1, "invalid HID storage state flags");
        Require(!present || presenceKnown, "storage reports present without known presence");
        Require(!initialized || (configured && presenceKnown && present), "initialized storage is not present");
        Require(!cidValid || (initialized && sampleValid), "CID is valid without an initialized sample");
        Require(!capacityValid || (initialized && sampleValid), "capacity is valid without an initialized sample");
        Require(!initialized || (clock != 0 && speed is 1 or 2), "initialized storage lacks active bus details");
        Require(initialized || (speed == 0 && clock == 0), "uninitialized storage has active bus details");
        Require(!capacityValid || blocks != 0, "valid capacity is zero");
        Require(capacityValid || blocks == 0, "invalid capacity has a block count");
        Require(cidValid || cid.IndexOfAnyExcept((byte)0) < 0, "invalid CID field is nonzero");
        Require(!sampleValid || (configured && component == 1
            && mode == (((identity.Capabilities & CapabilitySpi) != 0) ? 2 : 1)),
            "storage sample disagrees with selected transport");
        Require(sampleValid || ((flags & 0x3e) == 0 && component == 0 && mode == 0
            && clock == 0 && speed == 0 && blocks == 0 && cid.IndexOfAnyExcept((byte)0) < 0),
            "invalid storage sample contains card data");
        Require((mscEnabled == 0 && transition == 0) || !sampleValid,
            "MSC ownership requires an invalidated SD sample");

        SdCardIdentity? identityInfo = null;
        if (cidValid) identityInfo = ParseCid(cid);
        var memory = new MemoryStatusResponse(payload[1], configured, presenceKnown,
            presenceKnown ? present : null, initialized, component == 1 ? "sd-card" : null,
            mode switch { 1 => "sdio", 2 => "spi", _ => null }, initialized ? clock : null,
            capacityValid ? blocks : null, capacityValid ? (ulong)blocks * 512 : null,
            identityInfo, sampleValid, speed);
        var msc = new MscStatusResponse(mscEnabled != 0, transition != 0, rejected != 0);
        return new(null, memory, msc, null, FilesystemName(filesystem));
    }

    public static MemoryStatusResponse ParseMemoryReport(ReadOnlySpan<byte> report) =>
        ParseStorageReport(report, new HardwareInfoResponse("PicoForge", "Unknown", 0, "",
            Capabilities: CapabilitySdio)).Memory!;

    private static SdCardIdentity ParseCid(ReadOnlySpan<byte> cid)
    {
        var oem = Encoding.ASCII.GetString(cid.Slice(1, 2));
        var product = Encoding.ASCII.GetString(cid.Slice(3, 5));
        Require(oem.All(character => character is >= ' ' and <= '~')
            && product.All(character => character is >= ' ' and <= '~'), "invalid text in SD CID");
        var revision = cid[8];
        var date = ((cid[13] & 0x0f) << 8) | cid[14];
        var month = date & 0x0f;
        Require((cid[13] & 0xf0) == 0 && month is >= 1 and <= 12, "invalid SD CID manufacturing date");
        return new(cid[0], oem, product, $"{revision >> 4}.{revision & 0x0f}",
            BinaryPrimitives.ReadUInt32BigEndian(cid[9..]), 2000 + (date >> 4), month);
    }

    public static SelfTestStatusResponse ParseSelfTestReport(ReadOnlySpan<byte> report)
    {
        var payload = InputPayload(report, SelfTestReportId, SelfTestPayloadSize);
        var state = payload[2];
        var flags = payload[3];
        var target = ReadUInt32LittleEndian(payload[4..]);
        var written = ReadUInt32LittleEndian(payload[8..]);
        var read = ReadUInt32LittleEndian(payload[12..]);
        var mismatches = ReadUInt32LittleEndian(payload[24..]);
        Require(payload[0] == ReportVersion && state <= 8 && (flags & ~3) == 0 && payload[31] == 0
            && payload[28] <= 11 && payload[29] <= 19 && payload[30] <= 11
            && target <= 1024u * 1024u * 1024u && written <= target && read <= written
            && (state != 6 || (target != 0 && written == target && read == target && mismatches == 0
                && (flags & 1) == 0 && payload[28] == 0)), "invalid HID self-test report");
        var name = state switch
        {
            0 => "idle", 1 => "queued", 2 => "mounting", 3 => "writing", 4 => "reading",
            5 => "cleanup", 6 => "passed", 7 => "failed", _ => "cancelled"
        };
        return new(payload[1], name, target, written, read, ReadUInt32LittleEndian(payload[16..]),
            ReadUInt32LittleEndian(payload[20..]), mismatches, payload[28], payload[29], payload[30],
            (flags & 1) != 0, (flags & 2) != 0);
    }

    public static byte[] BuildControlRequest(byte transactionId, byte opcode, byte flags, ReadOnlySpan<byte> arguments)
    {
        Require(transactionId != 0, "transaction ID must be nonzero");
        Require(arguments.Length == 5, "control command requires five argument bytes");
        var report = new byte[ControlFeatureSize];
        report[0] = ControlReportId;
        report[1] = ReportVersion;
        report[2] = transactionId;
        report[3] = opcode;
        report[4] = flags;
        arguments.CopyTo(report.AsSpan(5));
        return report;
    }

    public static ControlResult ParseControlResult(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, ControlReportId, ControlPayloadSize);
        var result = new ControlResult(payload[1], payload[2], payload[3], payload[4], payload[5..9].ToArray());
        Require(payload[0] == ReportVersion && result.Status <= 6 && result.ErrorReason <= 7,
            "invalid HID control result version, status, or error");
        if (result.Status == 0)
        {
            Require(result.TransactionId == 0 && result.Opcode == 0 && result.ErrorReason == 0
                && result.ResultData.All(value => value == 0), "invalid idle control result");
            return result;
        }
        Require(result.TransactionId != 0, "control result has a zero transaction ID");
        Require(result.Status switch
        {
            1 or 2 => result.ErrorReason == 0,
            3 => result.ErrorReason is 1 or 2 or 3,
            4 => result.ErrorReason is 4 or 7,
            5 => result.ErrorReason == 5,
            6 => result.ErrorReason == 6,
            _ => false
        }, "invalid control status/error combination");
        Require(result.Opcode is 1 or 2 or 3 or 4 or 6
            || (result.Opcode is not 0 && result.Status == 4 && result.ErrorReason == 7),
            "control result has an unsupported opcode/status combination");
        Require(result.Opcode switch
        {
            1 => result.Status is 1 or 2 or 3 or 4 or 6,
            2 => result.Status is 1 or 2 or 3 or 4 or 5 or 6,
            3 => result.Status is 1 or 2 or 3 or 5 or 6,
            4 => result.Status is 1 or 2 or 3 or 4 or 5 or 6,
            6 => result.Status is 1 or 2 or 3 or 4 or 5 or 6,
            _ => result.Status == 4
        }, "control result status is invalid for its opcode");
        if (result.Opcode == 6 && result.Status == 2)
            Require(result.ResultData[2] <= 1 && result.ResultData[3] == 0,
                "invalid saved-settings result data");
        else
            Require(result.ResultData.All(value => value == 0), "unexpected control result data");
        return result;
    }

    public static void RequireCompleted(ControlResult result, byte transactionId, byte opcode)
    {
        Require(result.TransactionId == transactionId && result.Opcode == opcode,
            "HID control result does not match the submitted transaction");
        if (result.Status == 2) return;
        var description = result.ErrorReason switch
        {
            1 => "malformed command fields",
            2 => "confirmation required",
            3 => "invalid command argument",
            4 => "command capability unavailable",
            5 => "device is busy",
            6 => "device operation failed",
            7 => "unsupported command",
            _ => "command did not complete"
        };
        throw new InvalidOperationException($"HID command rejected ({result.Status}/{result.ErrorReason}): {description}");
    }

    public static string? FilesystemName(byte format) => format switch
    {
        0 => null, 1 => "fat12", 2 => "fat16", 3 => "fat32", 4 => "exfat",
        _ => throw new InvalidOperationException("unknown HID filesystem format")
    };

    private static ReadOnlySpan<byte> InputPayload(ReadOnlySpan<byte> report, byte reportId, int size)
    {
        Require(report.Length >= size + 1 && report[0] == reportId,
            "unexpected HID input report length or ID");
        Require(report[(size + 1)..].IndexOfAnyExcept((byte)0) < 0,
            "unexpected HID input report padding");
        return report.Slice(1, size);
    }

    private static ReadOnlySpan<byte> FeaturePayload(ReadOnlySpan<byte> report, byte reportId, int size)
    {
        var offset = report.Length >= size + 2 && report[0] == 0 && report[1] == reportId ? 2
            : report.Length >= size + 2 && report[0] == reportId && report[1] == reportId
                && report[2] == ReportVersion ? 2
            : report.Length >= size + 1 && report[0] == reportId ? 1
            : -1;
        Require(offset >= 0 && report.Length >= offset + size,
            "unexpected HID feature report length or ID");
        Require(report[(offset + size)..].IndexOfAnyExcept((byte)0) < 0,
            "unexpected HID feature report padding");
        return report.Slice(offset, size);
    }

    private static uint ReadUInt32LittleEndian(ReadOnlySpan<byte> source) =>
        BinaryPrimitives.ReadUInt32LittleEndian(source);

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
