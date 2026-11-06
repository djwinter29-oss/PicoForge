// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

using System.Buffers.Binary;
using System.Text;
using System.Text.Json.Serialization;

namespace PicoForge;

/// <summary>Device identity and capabilities from HID Feature report 1.</summary>
public sealed record HardwareInfoResponse(string ProductName, string Mcu, byte McuId,
    string SerialNumber, string? FirmwareVersion = null, ushort ProductId = 0,
    byte BoardModel = 0, byte Capabilities = 0, uint RamTotalBytes = 0, uint RamReservedBytes = 0)
{
    public bool WirelessSupported => (Capabilities & HidProtocol.CapabilityWireless) != 0;
}

/// <summary>Persisted next-boot clock, transport speed profile and radio enables (bit 0 Wi-Fi, bit 1 BLE).</summary>
public sealed record BootSettingsResponse(int SystemClockMhz,
    [property: JsonPropertyName("sdcard_profile")] int SdCardProfile, int Radios);

/// <summary>GPIO topology and persisted configuration from HID Feature report 2.</summary>
public sealed record BoardConfigurationResponse(
    [property: JsonPropertyName("stored_sysclk_profile")] ushort StoredSystemClockMhz,
    [property: JsonPropertyName("stored_sdcard_profile")] byte StoredSdCardProfile,
    byte MonitorUartIndex, byte MonitorUartTxGpio, byte MonitorUartRxGpio, byte[] StorageGpios,
    byte BoardFlags, byte ResetCause, byte LedGpio, byte CardDetectGpio)
{
    /// <summary>Next-boot values saved in this session; the stored values apply until a save replaces them.</summary>
    [JsonIgnore] public BootSettingsResponse? NextBoot { get; init; }
    public int SystemClockMhz => NextBoot?.SystemClockMhz ?? StoredSystemClockMhz;
    [JsonPropertyName("sdcard_profile")] public int SdCardProfile => NextBoot?.SdCardProfile ?? StoredSdCardProfile;
    public int Radios => NextBoot?.Radios ?? BoardFlags >> HidProtocol.BoardRadioShift & 3;
    public bool TcpCliEnabled => (BoardFlags & HidProtocol.BoardFlagTcpCli) != 0;
}

/// <summary>Dynamic telemetry reported periodically by HID Input report 3.</summary>
public sealed record RuntimeStatusResponse(byte Sequence, uint UptimeMs, uint SystemClockHz,
    bool UsbMounted, bool CdcOpen, bool TemperatureValid, short? TemperatureCentidegrees,
    bool LedAvailable, bool LedOn, ushort? CpuUsageCore0BasisPoints = null,
    ushort? CpuUsageCore1BasisPoints = null, uint? RamUsedBytes = null, uint? RamTotalBytes = null,
    ulong UptimeMilliseconds = 0);

/// <summary>Card identity decoded from the SD CID register.</summary>
public sealed record SdCardIdentity(byte ManufacturerId, string OemId, string ProductName,
    string ProductRevision, uint SerialNumber, int ManufactureYear, int ManufactureMonth);

/// <summary>SD, filesystem, and active speed state from HID Input report 10.</summary>
public sealed record MemoryStatusResponse(byte Sequence, bool Configured, bool PresenceKnown,
    bool? Present, bool Initialized, string? ComponentType, string? Mode, uint? BusClockHz,
    uint? BlockCount, ulong? CapacityBytes, SdCardIdentity? Manufacturer,
    bool SampleValid = true, byte SpeedMode = 0)
{
    /// <summary>USB-drive ownership from the same storage report.</summary>
    public MscStatusResponse? Msc { get; init; }
    /// <summary>Mounted filesystem name from the same storage report.</summary>
    public string? FilesystemFormat { get; init; }
}

public sealed record MscStatusResponse(bool Enabled, bool TransitionPending, bool RequestRejected);

/// <summary>Verified SD-card file benchmark progress, elapsed times, and cleanup state.</summary>
public sealed record SelfTestStatusResponse(byte Sequence, string State, uint TargetBytes,
    uint WrittenBytes, uint ReadBytes, uint WriteElapsedMs, uint ReadElapsedMs, uint MismatchCount,
    byte Error, byte FilesystemResult, bool FileRemains, bool VolumeMounted)
{
    public bool Active => State is "queued" or "preparing" or "writing" or "reading" or "cleanup";
    public double? WriteBytesPerSecond => WriteElapsedMs == 0 ? null : WrittenBytes * 1000.0 / WriteElapsedMs;
    public double? ReadBytesPerSecond => ReadElapsedMs == 0 ? null : ReadBytes * 1000.0 / ReadElapsedMs;
    public double ProgressPercent =>
        TargetBytes == 0 ? 0 : ((double)WrittenBytes + ReadBytes) * 100 / (2.0 * TargetBytes);
}

/// <summary>One parsed HID input report; exactly one section is set.</summary>
internal sealed record InputReport(RuntimeStatusResponse? Runtime, MemoryStatusResponse? Memory,
    SelfTestStatusResponse? SelfTest = null);

/// <summary>
/// Current HID connection and the most recent hardware/runtime reports, stored in the JSON shape the Python server
/// sends: USB-drive ownership and filesystem in <c>memory</c>, next-boot settings in <c>board_configuration</c>.
/// </summary>
public sealed record StatusResponse(bool Connected, string? Error, HardwareInfoResponse? Hardware = null,
    RuntimeStatusResponse? Runtime = null, DateTimeOffset? UpdatedAt = null, MemoryStatusResponse? Memory = null,
    SelfTestStatusResponse? SelfTest = null, BoardConfigurationResponse? BoardConfiguration = null,
    bool TelemetryStale = true, bool StorageStale = true, bool SelfTestStale = true,
    bool CommandOutcomeUncertain = false, bool RebootPending = false, bool Reconnecting = false,
    string? WirelessPhase = null, WirelessStatusResponse? Wireless = null, string? WirelessError = null,
    bool CommandResyncSupported = true, string? SettingsMessage = null, bool WirelessCleanupRequired = false)
{
    public bool SettingsSupported => BoardConfiguration is not null;
}

internal readonly record struct ControlCommandReference(byte TransactionId, Opcode Opcode);
internal readonly record struct WirelessOperationReference(string Kind, ushort Generation);
internal sealed record CommandResyncResult(ControlResult? ControlResult, WirelessStatusResponse? Wireless);
public sealed record ControlResultResponse(byte Version, byte TransactionId, byte Opcode, byte StatusId,
    string Status, byte ErrorReason, string Error, byte Detail);
public sealed record CommandResyncResponse(bool Uncertain, ControlResultResponse? ControlResult = null,
    WirelessStatusResponse? Wireless = null);

/// <summary>Feature 20 opcodes, named as <c>PF_HID_OPCODE_*</c> in firmware <c>usb_hid.h</c>.</summary>
internal enum Opcode : byte
{
    /// <summary>Set the board LED.</summary>
    Led = 1,
    /// <summary>Reboot after confirmation.</summary>
    Reboot = 2,
    /// <summary>Switch USB mass-storage ownership.</summary>
    Msc = 3,
    /// <summary>Start or cancel the SD file benchmark.</summary>
    SelfTest = 4,
    /// <summary>Save next-boot settings and optionally commit staged wireless values.</summary>
    SaveSettings = 5,
    /// <summary>Stage a Wi-Fi SSID.</summary>
    StageSsid = 6,
    /// <summary>Stage a Wi-Fi password.</summary>
    StagePassword = 7,
    /// <summary>Stage the Control key.</summary>
    StageKey = 8,
    /// <summary>Join the staged Wi-Fi pair as a test.</summary>
    WifiTest = 9,
    /// <summary>Cancel a Wi-Fi test and drop the test credentials.</summary>
    WifiCancel = 10,
    /// <summary>Start a Wi-Fi scan.</summary>
    WifiScan = 11,
    /// <summary>Read one scan result entry.</summary>
    WifiEntry = 12,
    /// <summary>Sign a TCP CLI authentication challenge.</summary>
    TcpProof = 13
}

/// <summary>Feature 20 command statuses, matching <c>PF_HID_COMMAND_*</c>.</summary>
internal enum ControlStatus : byte
{
    /// <summary>No command result has been published.</summary>
    Idle = 0,
    /// <summary>Accepted; the result will be published later.</summary>
    Pending = 1,
    /// <summary>Command completed successfully.</summary>
    Completed = 2,
    /// <summary>Request was malformed or out of range.</summary>
    Invalid = 3,
    /// <summary>Opcode or capability is unavailable.</summary>
    Unsupported = 4,
    /// <summary>Device state currently blocks the command.</summary>
    Busy = 5,
    /// <summary>Accepted command failed while executing.</summary>
    Failed = 6
}

/// <summary>Feature 20 command errors, matching <c>PF_HID_ERROR_*</c>.</summary>
internal enum ControlError : byte
{
    /// <summary>No error.</summary>
    None = 0,
    /// <summary>Reserved bits, flags or trailing arguments are invalid.</summary>
    Malformed = 1,
    /// <summary>A required confirmation flag is missing.</summary>
    Confirmation = 2,
    /// <summary>An argument is out of range or not applicable.</summary>
    Argument = 3,
    /// <summary>The device lacks the capability.</summary>
    Capability = 4,
    /// <summary>Storage or benchmark activity blocks the command.</summary>
    Busy = 5,
    /// <summary>The device operation failed.</summary>
    Operation = 6,
    /// <summary>The opcode is unsupported.</summary>
    Opcode = 7
}

/// <summary>One Feature 20 result; <see cref="Opcode"/> may hold an unassigned value echoed by firmware.</summary>
internal sealed record ControlResult(byte TransactionId, Opcode Opcode, ControlStatus Status, ControlError ErrorReason,
    byte[] ResultData);

internal static partial class HidProtocol
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
    public const int ControlPayloadSize = 68;
    public const int IdentityFeatureSize = IdentityPayloadSize + 1;
    public const int ConfigurationFeatureSize = ConfigurationPayloadSize + 1;
    public const int ControlFeatureSize = ControlPayloadSize + 1;
    public const byte CapabilityLed = 1 << 0;
    public const byte CapabilityReboot = 1 << 1;
    public const byte CapabilitySettings = 1 << 2;
    public const byte CapabilitySpi = 1 << 3;
    public const byte CapabilitySdio = 1 << 4;
    public const byte CapabilitySelfTest = 1 << 5;
    public const byte CapabilityForcedSave = 1 << 6;
    public const byte CapabilityWireless = 1 << 7;
    /// <summary>Feature 2 board flags (<c>PF_HID_BOARD_FLAG_*</c>); bits 3..4 hold the saved radio switches.</summary>
    public const byte BoardFlagCardDetect = 1 << 0;
    public const byte BoardFlagLed = 1 << 1;
    public const byte BoardFlagMonitorUart = 1 << 2;
    public const byte BoardFlagWifi = 1 << 3;
    public const byte BoardFlagBle = 1 << 4;
    public const byte BoardFlagTcpCli = 1 << 5;
    public const int BoardRadioShift = 3;
    /// <summary>Opcode 5 switches byte: bits 0..1 radios, bit 2 TCP CLI.</summary>
    public const byte SwitchTcpCli = 1 << 2;
    /// <summary>Opcode 5 flags: confirm a save while the host owns storage; commit staged wireless values.</summary>
    public const byte SaveForceMsc = 1 << 0;
    public const byte SaveCommitWireless = 1 << 1;

    /// <summary>Saved system-clock requests in MHz; 0 selects the MCU default.</summary>
    public static bool IsValidClock(int mhz) => mhz is 0 or 150 or 200 or 250;

    /// <summary>
    /// Throws <see cref="ArgumentException"/> unless every next-boot value is one the firmware accepts.
    /// </summary>
    public static void ValidateSettings(BootSettingsResponse settings)
    {
        if (!IsValidClock(settings.SystemClockMhz) || settings.SdCardProfile is < 0 or > 1 ||
            settings.Radios is < 0 or > 3)
            throw new ArgumentException("Unsupported next-boot settings value");
    }

    /// <summary>Opcode 5 arguments: clock (LE16), SD profile and the switches byte.</summary>
    public static byte[] EncodeSettings(BootSettingsResponse settings, bool tcpCli)
    {
        var arguments = new byte[64];
        BinaryPrimitives.WriteUInt16LittleEndian(arguments, (ushort)settings.SystemClockMhz);
        arguments[2] = (byte)settings.SdCardProfile;
        arguments[3] = (byte)(settings.Radios | (tcpCli ? SwitchTcpCli : 0));
        return arguments;
    }

    /// <summary>
    /// True when an opcode 5 result echoes the sent arguments and commits exactly <paramref name="mask"/>.
    /// </summary>
    public static bool SettingsConfirmed(ControlResult result, ReadOnlySpan<byte> arguments, int mask) =>
        result.ResultData.AsSpan(0, 4).SequenceEqual(arguments[..4]) && result.ResultData[4] == mask;

    public static HardwareInfoResponse ParseIdentityReport(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, IdentityReportId, IdentityPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID identity report version");
        var productId = BinaryPrimitives.ReadUInt16LittleEndian(payload[1..]);
        Require(productId == 2, $"unsupported PicoForge product ID 0x{productId:X4}");
        var capabilities = payload[17];
        var spi = (capabilities & CapabilitySpi) != 0;
        var sdio = (capabilities & CapabilitySdio) != 0;
        Require(spi != sdio, "invalid HID capability flags");
        var mcuId = payload[14];
        var boardModel = payload[15];
        Require(boardModel switch
        {
            1 or 4 => mcuId == 1,
            2 or 3 => mcuId == 2,
            _ => true
        }, "board model and MCU do not match");
        var ramTotal = BinaryPrimitives.ReadUInt32LittleEndian(payload[18..]);
        var ramReserved = BinaryPrimitives.ReadUInt32LittleEndian(payload[22..]);
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
        const int wirelessFlags = BoardFlagWifi | BoardFlagBle | BoardFlagTcpCli;
        Require(IsValidClock(clock), "unsupported persisted system-clock profile");
        Require(sdProfile <= 1, "unsupported persisted SD-card profile");
        Require((flags & ~0x3F) == 0, "unknown board-configuration flags");
        Require((flags & wirelessFlags) == 0 || identity.WirelessSupported,
            "wireless service switches reported without wireless capability");
        Require((uartIndex <= 1 || uartIndex == byte.MaxValue)
            && ((tx == byte.MaxValue) == (rx == byte.MaxValue)), "invalid monitor UART assignment");
        Require(((flags & BoardFlagMonitorUart) == 0 || (uartIndex <= 1 && tx != byte.MaxValue
                && rx != byte.MaxValue)), "enabled monitor UART requires assigned pins");
        Require(((flags & BoardFlagCardDetect) != 0) == (detectGpio != byte.MaxValue)
            && ((flags & BoardFlagLed) != 0) == (ledGpio != byte.MaxValue)
            && ((flags & BoardFlagLed) == 0 || (identity.Capabilities & CapabilityLed) != 0),
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

    public static RuntimeStatusResponse ParseTelemetryReport(ReadOnlySpan<byte> report)
    {
        var payload = InputPayload(report, TelemetryReportId, TelemetryPayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID telemetry report version");
        var ledFlags = payload[1];
        var temperature = BinaryPrimitives.ReadInt16LittleEndian(payload[2..]);
        var uptime = BinaryPrimitives.ReadUInt64LittleEndian(payload[4..]);
        var core0 = BinaryPrimitives.ReadUInt16LittleEndian(payload[12..]);
        var core1 = BinaryPrimitives.ReadUInt16LittleEndian(payload[14..]);
        var clock = BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]);
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


    public static InputReport ParseStorageReport(ReadOnlySpan<byte> report, HardwareInfoResponse identity)
    {
        var payload = InputPayload(report, StorageReportId, StoragePayloadSize);
        Require(payload[0] == ReportVersion, "unsupported HID storage report version");
        var flags = payload[2];
        var component = payload[3];
        var mode = payload[4];
        var clock = BinaryPrimitives.ReadUInt32LittleEndian(payload[5..]);
        var speed = payload[9];
        var blocks = BinaryPrimitives.ReadUInt32LittleEndian(payload[10..]);
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
        return new(null, memory with
        {
            Msc = new(mscEnabled != 0, transition != 0, rejected != 0), FilesystemFormat = FilesystemName(filesystem)
        });
    }

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
        var target = BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]);
        var written = BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]);
        var read = BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]);
        var mismatches = BinaryPrimitives.ReadUInt32LittleEndian(payload[24..]);
        Require(payload[0] == ReportVersion && (flags & ~3) == 0 && payload[31] == 0
            && payload[28] <= 10 && payload[29] <= 19 && payload[30] == 0
            && target <= 1024u * 1024u * 1024u && written <= target && read <= written
            && (state != 6 || (target != 0 && written == target && read == target && mismatches == 0
                && (flags & 1) == 0 && payload[28] == 0)), "invalid HID self-test report");
        var name = state switch
        {
            0 => "idle", 1 => "queued", 2 => "preparing", 3 => "writing", 4 => "reading",
            5 => "cleanup", 6 => "passed", 7 => "failed", 8 => "cancelled", _ => $"unknown ({state})"
        };
        return new(payload[1], name, target, written, read,
            BinaryPrimitives.ReadUInt32LittleEndian(payload[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[20..]), mismatches, payload[28], payload[29],
            (flags & 1) != 0, (flags & 2) != 0);
    }

    public static byte[] BuildControlRequest(byte transactionId, Opcode opcode, byte flags,
        ReadOnlySpan<byte> arguments)
    {
        if (transactionId == 0 || arguments.Length != 64 ||
            (opcode == Opcode.SaveSettings && (flags & ~(SaveForceMsc | SaveCommitWireless)) != 0))
            throw new ArgumentException("Invalid HID control request fields");
        var report = new byte[ControlFeatureSize];
        report[0] = ControlReportId;
        report[1] = ReportVersion;
        report[2] = transactionId;
        report[3] = (byte)opcode;
        report[4] = flags;
        arguments.CopyTo(report.AsSpan(5));
        return report;
    }

    public static ControlResult ParseControlResult(ReadOnlySpan<byte> report)
    {
        var payload = FeaturePayload(report, ControlReportId, ControlPayloadSize);
        var result = new ControlResult(payload[1], (Opcode)payload[2], (ControlStatus)payload[3],
            (ControlError)payload[4],
            payload[5..].ToArray());
        Require(payload[0] == ReportVersion && (byte)result.Status <= (byte)ControlStatus.Failed &&
            (byte)result.ErrorReason <= (byte)ControlError.Opcode,
            "invalid HID control result version, status, or error");
        if (result.Status == ControlStatus.Idle)
        {
            Require(result.TransactionId == 0 && result.Opcode == (Opcode)0 &&
                result.ErrorReason == ControlError.None
                && result.ResultData.All(value => value == 0), "invalid idle control result");
            return result;
        }
        Require(result.TransactionId != 0, "control result has a zero transaction ID");
        Require(result.Status switch
        {
            ControlStatus.Pending or ControlStatus.Completed => result.ErrorReason == ControlError.None,
            ControlStatus.Invalid => result.ErrorReason is ControlError.Malformed or ControlError.Confirmation
                or ControlError.Argument,
            ControlStatus.Unsupported => result.ErrorReason is ControlError.Capability or ControlError.Opcode,
            ControlStatus.Busy => result.ErrorReason == ControlError.Busy,
            ControlStatus.Failed => result.ErrorReason == ControlError.Operation,
            _ => false
        }, "invalid control status/error combination");
        var assigned = Enum.IsDefined(result.Opcode);
        Require(assigned || (result.Opcode != (Opcode)0 && result.Status == ControlStatus.Unsupported &&
            result.ErrorReason == ControlError.Opcode),
            "control result has an unsupported opcode/status combination");
        Require(result.Opcode switch
        {
            Opcode.Led => result.Status is ControlStatus.Pending or ControlStatus.Completed
                or ControlStatus.Invalid or ControlStatus.Failed,
            Opcode.Reboot or Opcode.Msc => result.Status is ControlStatus.Pending or ControlStatus.Completed
                or ControlStatus.Invalid or ControlStatus.Busy or ControlStatus.Failed,
            Opcode.SelfTest or Opcode.SaveSettings or Opcode.WifiTest or Opcode.WifiScan =>
                result.Status is ControlStatus.Pending or ControlStatus.Completed or ControlStatus.Invalid
                    or ControlStatus.Unsupported or ControlStatus.Busy or ControlStatus.Failed,
            Opcode.StageSsid or Opcode.StagePassword or Opcode.StageKey or Opcode.WifiEntry =>
                result.Status is ControlStatus.Completed or ControlStatus.Invalid or ControlStatus.Unsupported
                    or ControlStatus.Busy or ControlStatus.Failed,
            Opcode.WifiCancel or Opcode.TcpProof => result.Status is ControlStatus.Completed
                or ControlStatus.Invalid or ControlStatus.Unsupported or ControlStatus.Failed,
            _ => result.Status == ControlStatus.Unsupported && result.ErrorReason == ControlError.Opcode
        }, "control result status is invalid for its opcode");
        Require(!assigned || result.Status != ControlStatus.Unsupported ||
            result.ErrorReason == ControlError.Capability,
            "assigned opcode reported as unassigned");
        Require(result.Status != ControlStatus.Invalid || result.ErrorReason != ControlError.Confirmation ||
            result.Opcode is Opcode.Reboot or Opcode.Msc,
            "confirmation error is invalid for this opcode");
        Require(result.Opcode != Opcode.Reboot || result.Status != ControlStatus.Invalid ||
            result.ErrorReason is ControlError.Malformed or ControlError.Confirmation,
            "invalid reboot error");
        if (result.Status is ControlStatus.Invalid or ControlStatus.Busy or ControlStatus.Failed)
        {
            var detail = result.ResultData[0];
            Require(result.ResultData.AsSpan(1).IndexOfAnyExcept((byte)0) < 0 &&
                ValidControlDetail(result, detail), "invalid control detail");
        }
        else if (result.Status == ControlStatus.Completed && result.Opcode == Opcode.SaveSettings)
        {
            Require(IsValidClock(BinaryPrimitives.ReadUInt16LittleEndian(result.ResultData)) &&
                result.ResultData[2] <= 1 && result.ResultData[3] <= 7 && result.ResultData[4] <= 7 &&
                result.ResultData.AsSpan(5).IndexOfAnyExcept((byte)0) < 0, "invalid saved-settings result data");
        }
        else if (result.Status == ControlStatus.Completed && result.Opcode == Opcode.WifiEntry)
        {
            var data = result.ResultData;
            Require(BinaryPrimitives.ReadUInt16LittleEndian(data) != 0 && data[2] < 18 && data[3] <= 32 &&
                data.AsSpan(4 + data[3], 32 - data[3]).IndexOfAnyExcept((byte)0) < 0 &&
                data[37] <= 7 && data.AsSpan(38).IndexOfAnyExcept((byte)0) < 0, "invalid scan entry result");
        }
        else if (result.Status == ControlStatus.Completed && result.Opcode == Opcode.TcpProof)
        {
            Require(result.ResultData.AsSpan(16).IndexOfAnyExcept((byte)0) < 0, "invalid TCP CLI proof padding");
        }
        else
            Require(result.ResultData.All(value => value == 0), "unexpected control result data");
        return result;
    }

    private static bool ValidControlDetail(ControlResult result, byte detail)
    {
        var op = result.Opcode;
        if (detail == 0)
            return !(op == Opcode.SelfTest &&
                (result.Status is ControlStatus.Busy or ControlStatus.Failed ||
                 result.Status == ControlStatus.Invalid && result.ErrorReason == ControlError.Argument));
        if (result.Status == ControlStatus.Invalid && result.ErrorReason == ControlError.Argument)
            return detail switch
            {
                3 => op == Opcode.SelfTest,
                16 => op == Opcode.StageSsid, 17 => op == Opcode.StagePassword, 18 => op == Opcode.StageKey,
                19 or 20 or 21 => op is Opcode.SaveSettings or Opcode.WifiTest,
                22 or 23 => op == Opcode.WifiEntry, 24 => op == Opcode.TcpProof, 27 => op == Opcode.SaveSettings,
                _ => false
            };
        if (result.Status == ControlStatus.Busy)
            return detail switch
            {
                1 or 2 => op is Opcode.Reboot or Opcode.Msc or Opcode.SelfTest or Opcode.SaveSettings,
                25 => op is Opcode.SaveSettings or Opcode.StageSsid or Opcode.StagePassword or Opcode.StageKey
                    or Opcode.WifiTest or Opcode.WifiScan,
                26 => op is Opcode.SaveSettings or Opcode.WifiTest or Opcode.WifiScan or Opcode.WifiEntry,
                _ => false
            };
        return result.Status == ControlStatus.Failed && op == Opcode.SelfTest && detail is 4 or 5 or 6 or 7 or 10;
    }

    public static void RequireCompleted(ControlResult result, byte transactionId, Opcode opcode)
    {
        Require(result.TransactionId == transactionId && result.Opcode == opcode,
            "HID control result does not match the submitted transaction");
        if (result.Status == ControlStatus.Completed) return;
        var description = result.ErrorReason switch
        {
            ControlError.Malformed => "malformed command fields",
            ControlError.Confirmation => "confirmation required",
            ControlError.Argument => "invalid command argument",
            ControlError.Capability => "command capability unavailable",
            ControlError.Busy => "device is busy",
            ControlError.Operation => "device operation failed",
            ControlError.Opcode => "unsupported command",
            _ => "command did not complete"
        };
        if (result.ResultData[0] >= 16)
            description = result.ResultData[0] switch
            {
                16 => "invalid SSID length", 17 => "invalid password", 18 => "invalid Control key length",
                19 => "credential pair is incomplete or unsafe", 20 => "staged wireless values expired",
                21 => "no eligible staged wireless values", 22 => "scan generation is stale",
                23 => "scan index is out of range", 24 => "no Control key is saved",
                25 => "a Wi-Fi test is active", 26 => "a Wi-Fi scan is active",
                27 => "staged wireless values require explicit commit", _ => description
            };
        throw new BoardStateException(
            $"HID command rejected ({(byte)result.Status}/{(byte)result.ErrorReason}): {description}");
    }

    public static string? FilesystemName(byte format) => format switch
    {
        0 => null, 1 => "fat12", 2 => "fat16", 3 => "fat32", 4 => "exfat",
        _ => throw new InvalidDataException("unknown HID filesystem format")
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
        Require(report.Length == size + 1 && report[0] == reportId,
            "unexpected HID feature report length or ID");
        return report.Slice(1, size);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException(message);
    }
}
