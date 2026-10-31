using System.Buffers.Binary;
using HidSharp;

namespace PicoForge;

public sealed record HidBoard(string DevicePath, string? SerialNumber, string ProductName);

internal sealed class PicoForgeHid : IHidCommands, IDisposable
{
    private HidDevice device = null!;
    private HidStream stream = null!;
    private readonly Func<byte, int, byte[]> readFeature;
    private readonly Action<byte[]> writeFeature;
    private readonly Action<string> reopen;
    private HardwareInfoResponse? identity;
    private byte transactionId;
    public bool CommandOutcomeUncertain { get; private set; }

    public PicoForgeHid(string? serialNumber = null, string? devicePath = null)
    {
        if (serialNumber is not null && devicePath is not null)
            throw new ArgumentException("--serial and --device-path cannot be used together");
        var candidates = MatchingDevices()
            .Where(candidate => devicePath is null || candidate.DevicePath == devicePath)
            .Where(candidate => serialNumber is null || candidate.GetSerialNumber() == serialNumber)
            .ToArray();
        if (candidates.Length == 0)
            throw new InvalidOperationException("PicoForge HID interface not found; check USB connection and HID permissions");
        if (candidates.Length != 1)
            throw new InvalidOperationException("Multiple PicoForge HID interfaces matched; connect one board at a time");

        device = candidates[0];
        stream = device.Open();
        stream.ReadTimeout = 250;
        stream.WriteTimeout = 1000;
        readFeature = GetFeatureReport;
        writeFeature = report => stream.SetFeature(report);
        reopen = ReopenSameBoard;
    }

    internal PicoForgeHid(HardwareInfoResponse identity, Func<byte, int, byte[]> readFeature,
        Action<byte[]> writeFeature, Action<string> reopen)
    {
        this.identity = identity;
        this.readFeature = readFeature;
        this.writeFeature = writeFeature;
        this.reopen = reopen;
    }

    public static HidBoard[] Discover() => MatchingDevices().Select(candidate => new HidBoard(
        candidate.DevicePath,
        string.IsNullOrEmpty(candidate.GetSerialNumber()) ? null : candidate.GetSerialNumber(),
        string.IsNullOrEmpty(candidate.GetProductName()) ? "PicoForge" : candidate.GetProductName())).ToArray();

    private static IEnumerable<HidDevice> MatchingDevices() =>
        DeviceList.Local.GetHidDevices(HidProtocol.VendorId, HidProtocol.ProductId)
            .Concat(DeviceList.Local.GetHidDevices(HidProtocol.VendorId, HidProtocol.ProductIdAlternate))
            .DistinctBy(candidate => candidate.DevicePath)
            .Where(HasPicoForgeUsage);

    public HardwareInfoResponse ReadInfo()
    {
        var report = readFeature(HidProtocol.IdentityReportId, HidProtocol.IdentityFeatureSize);
        identity = HidProtocol.ParseIdentityReport(report);
        return identity;
    }

    public BoardConfigurationResponse ReadConfiguration()
    {
        identity ??= ReadInfo();
        return HidProtocol.ParseConfigurationReport(
            readFeature(HidProtocol.ConfigurationReportId, HidProtocol.ConfigurationFeatureSize), identity);
    }

    public BootSettingsResponse ReadSettings() => HidProtocol.ToBootSettings(ReadConfiguration());

    public RuntimeStatusResponse? ReadStatus()
    {
        var deadline = Environment.TickCount64 + 1000;
        while (Environment.TickCount64 < deadline)
        {
            var report = ReadInputReport();
            if (report?.Runtime is not null) return report.Runtime;
        }
        return null;
    }

    public InputReport? ReadInputReport()
    {
        var report = new byte[Math.Max(HidProtocol.SelfTestPayloadSize + 1, device.GetMaxInputReportLength())];
        try
        {
            var count = stream.Read(report, 0, report.Length);
            if (count == 0) return null;
            var data = report.AsSpan(0, count);
            identity ??= ReadInfo();
            return data[0] switch
            {
                HidProtocol.TelemetryReportId => new(HidProtocol.ParseTelemetryReport(data), null, null),
                HidProtocol.StorageReportId => HidProtocol.ParseStorageReport(data, identity),
                HidProtocol.SelfTestReportId when (identity.Capabilities & HidProtocol.CapabilitySelfTest) != 0
                    => new(null, null, null, HidProtocol.ParseSelfTestReport(data)),
                HidProtocol.SelfTestReportId => throw new InvalidOperationException(
                    "firmware published self-test status without advertising the capability"),
                _ => throw new InvalidOperationException($"unexpected HID input report ID {data[0]}")
            };
        }
        catch (TimeoutException)
        {
            return null;
        }
    }

    public void SetLed(bool on)
    {
        EnsureCapability(HidProtocol.CapabilityLed, "LED control");
        var arguments = new byte[5];
        arguments[0] = on ? (byte)1 : (byte)0;
        SendCommand(1, 0, arguments);
    }

    public void SetMsc(bool enabled, bool force = false)
    {
        if (force && enabled) throw new ArgumentException("Force return requires disabling MSC");
        identity ??= ReadInfo();
        if ((identity.Capabilities & (HidProtocol.CapabilitySpi | HidProtocol.CapabilitySdio)) == 0)
            throw new NotSupportedException("MSC control is unavailable on this firmware");
        var arguments = new byte[5];
        arguments[0] = force ? (byte)2 : enabled ? (byte)1 : (byte)0;
        SendCommand(3, force ? (byte)1 : (byte)0, arguments);
    }

    public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
    {
        EnsureCapability(HidProtocol.CapabilitySettings, "persistent settings");
        if (force) EnsureCapability(HidProtocol.CapabilityForcedSave, "forced settings save");
        ValidateSettings(settings);
        var arguments = new byte[5];
        BinaryPrimitives.WriteUInt16LittleEndian(arguments, (ushort)settings.SystemClockMhz);
        arguments[2] = (byte)settings.SdCardProfile;
        arguments[3] = (byte)settings.Radios;
        var result = SendCommand(6, force ? (byte)1 : (byte)0, arguments);
        var saved = new BootSettingsResponse(BinaryPrimitives.ReadUInt16LittleEndian(result.ResultData),
            result.ResultData[2], result.ResultData[3]);
        if (saved != settings)
        {
            if (force) CommandOutcomeUncertain = true;
            throw new InvalidOperationException("Firmware did not confirm the selected next-boot settings");
        }
        return saved;
    }

    public void Reboot()
    {
        EnsureCapability(HidProtocol.CapabilityReboot, "reboot");
        SendCommand(2, 1, new byte[5]);
    }

    public WirelessStatusResponse ReadWireless()
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless provisioning");
        return HidProtocol.ParseWirelessStatus(readFeature(HidProtocol.WirelessReportId, HidProtocol.WirelessFeatureSize));
    }

    public WirelessStatusResponse SetWireless(WirelessRequest settings)
    {
        if (CommandOutcomeUncertain)
            throw new InvalidOperationException("A prior command outcome is uncertain; further writes are blocked");
        var previous = ReadWireless();
        if (previous.Status is "pending" or "busy")
            throw new InvalidOperationException("Wireless provisioning is busy");
        transactionId = transactionId == byte.MaxValue ? (byte)1 : (byte)(transactionId + 1);
        var request = HidProtocol.BuildWirelessRequest(settings, transactionId);
        CommandOutcomeUncertain = true;
        writeFeature(request);
        var deadline = Environment.TickCount64 + 5000;
        while (Environment.TickCount64 < deadline)
        {
            var result = ReadWireless();
            if (result.TransactionId == transactionId)
            {
                if (result.Status == "completed")
                {
                    if (result.WifiEnabled != settings.WifiEnabled || result.BleEnabled != settings.BleEnabled ||
                        result.Ssid != settings.Ssid ||
                        result.PasswordConfigured != (settings.Password is not null || previous.PasswordConfigured) ||
                        result.BleKeyConfigured != (settings.BleKey is not null || previous.BleKeyConfigured))
                        throw new InvalidOperationException(
                            "Wireless saved configuration was not confirmed; outcome is uncertain");
                    CommandOutcomeUncertain = false;
                    return result;
                }
                if (result.Status is "invalid" or "unsupported" or "busy" or "failed")
                {
                    CommandOutcomeUncertain = false;
                    throw new InvalidOperationException($"Wireless provisioning {result.Status}");
                }
            }
            Thread.Sleep(20);
        }
        throw new TimeoutException("Wireless provisioning outcome is uncertain; command was not resent");
    }

    public void SetSelfTest(string action, uint sizeMib)
    {
        EnsureCapability(HidProtocol.CapabilitySelfTest, "SD self-test");
        if (action is not ("start" or "cancel"))
            throw new ArgumentException("Self-test action must be start or cancel", nameof(action));
        if (action == "cancel") sizeMib = 0;
        else if (sizeMib is < 1 or > 1024)
            throw new ArgumentOutOfRangeException(nameof(sizeMib), "Self-test size must be 1..1024 MiB");
        var arguments = new byte[5];
        arguments[0] = action == "start" ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(arguments.AsSpan(1), sizeMib);
        SendCommand(4, 0, arguments);
    }

    public void Dispose() => stream?.Dispose();

    private byte[] GetFeatureReport(byte reportId, int requiredSize)
    {
        var report = new byte[Math.Max(requiredSize, device.GetMaxFeatureReportLength() + 1)];
        report[0] = reportId;
        stream.GetFeature(report);
        return report;
    }

    private ControlResult SendCommand(byte opcode, byte flags, byte[] arguments)
    {
        if (CommandOutcomeUncertain)
            throw new InvalidOperationException("A prior HID command outcome is uncertain; further writes are blocked");
        identity ??= ReadInfo();
        transactionId = transactionId == byte.MaxValue ? (byte)1 : (byte)(transactionId + 1);
        if (transactionId == 0) transactionId = 1;
        var request = HidProtocol.BuildControlRequest(transactionId, opcode, flags, arguments);
        var forcedSave = opcode == 6 && flags == 1;
        CommandOutcomeUncertain = true;
        var disconnected = false;
        try { writeFeature(request); }
        catch (Exception error) when (forcedSave && error is IOException or TimeoutException)
        {
            // SET may have reached firmware before USB detached. Only read its original result from here.
            disconnected = true;
        }

        var deadline = Environment.TickCount64 + (forcedSave ? 5000 : 1000);
        while (Environment.TickCount64 < deadline)
        {
            byte[] response;
            try
            {
                if (disconnected)
                {
                    reopen(identity.SerialNumber);
                    disconnected = false;
                }
                response = readFeature(HidProtocol.ControlReportId, HidProtocol.ControlFeatureSize);
            }
            catch (Exception error) when (forcedSave && error is IOException or TimeoutException)
            {
                disconnected = true;
                Thread.Sleep(20);
                continue;
            }
            var result = HidProtocol.ParseControlResult(response);
            if (result.TransactionId != transactionId || result.Opcode != opcode)
                throw new InvalidOperationException("HID control result was replaced by another transaction");
            if (result.Status != 1)
            {
                CommandOutcomeUncertain = false;
                HidProtocol.RequireCompleted(result, transactionId, opcode);
                return result;
            }
            Thread.Sleep(20);
        }
        throw new TimeoutException(forcedSave
            ? $"Forced save outcome is uncertain for transaction {transactionId}; the command was not resent"
            : $"Timed out waiting for HID command {opcode} transaction {transactionId}");
    }

    /// <summary>Replace a detached handle only after Feature 1 proves the original board UID.</summary>
    private void ReopenSameBoard(string uid)
    {
        stream.Dispose();
        foreach (var candidate in MatchingDevices())
        {
            HidStream? replacement = null;
            try
            {
                replacement = candidate.Open();
                replacement.ReadTimeout = 250;
                replacement.WriteTimeout = 1000;
                var report = new byte[Math.Max(HidProtocol.IdentityFeatureSize,
                    candidate.GetMaxFeatureReportLength() + 1)];
                report[0] = HidProtocol.IdentityReportId;
                replacement.GetFeature(report);
                if (HidProtocol.ParseIdentityReport(report).SerialNumber != uid) continue;
                device = candidate;
                stream = replacement;
                replacement = null;
                return;
            }
            catch (Exception error) when (error is IOException or TimeoutException or InvalidOperationException)
            {
                // A board may still be reconnecting; do not select an unverified identity.
            }
            finally { replacement?.Dispose(); }
        }
        throw new IOException("Waiting for the original PicoForge board UID to reconnect");
    }

    private void EnsureCapability(byte capability, string operation)
    {
        identity ??= ReadInfo();
        if ((identity.Capabilities & capability) == 0)
            throw new NotSupportedException($"{operation} is unavailable on this firmware");
    }

    private static void ValidateSettings(BootSettingsResponse settings)
    {
        if (settings.SystemClockMhz is not (0 or 150 or 200 or 250) || settings.SdCardProfile is < 0 or > 1 ||
            settings.Radios is < 0 or > 3)
            throw new ArgumentOutOfRangeException(nameof(settings), "Unsupported next-boot settings value");
    }

    private static bool HasPicoForgeUsage(HidDevice candidate)
    {
        try
        {
            return candidate.GetReportDescriptor().DeviceItems
                .Any(item => item.Usages.GetAllValues().Contains(HidProtocol.Usage));
        }
        catch (InvalidOperationException)
        {
            return false;
        }
        catch (NotSupportedException)
        {
            return false;
        }
    }
}
