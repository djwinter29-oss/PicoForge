using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
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
    private byte[] inputReport;
    private byte[] featureReport;
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
        inputReport = new byte[Math.Max(HidProtocol.SelfTestPayloadSize + 1, device.GetMaxInputReportLength())];
        featureReport = new byte[Math.Max(
            HidProtocol.ControlFeatureSize, device.GetMaxFeatureReportLength())];
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
        inputReport = new byte[HidProtocol.SelfTestPayloadSize + 1];
        featureReport = new byte[HidProtocol.ControlFeatureSize];
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

    public InputReport? ReadInputReport(int timeoutMilliseconds = 250)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(timeoutMilliseconds);
        var previousTimeout = stream.ReadTimeout;
        stream.ReadTimeout = timeoutMilliseconds;
        try
        {
            var count = stream.Read(inputReport, 0, inputReport.Length);
            if (count == 0) return null;
            var data = inputReport.AsSpan(0, count);
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
        finally
        {
            stream.ReadTimeout = previousTimeout;
        }
    }

    public void SetLed(bool on)
    {
        EnsureCapability(HidProtocol.CapabilityLed, "LED control");
        var arguments = new byte[64];
        arguments[0] = on ? (byte)1 : (byte)0;
        SendCommand(1, 0, arguments);
    }

    public void SetMsc(bool enabled, bool force = false)
    {
        if (force && enabled) throw new ArgumentException("Force return requires disabling MSC");
        identity ??= ReadInfo();
        if ((identity.Capabilities & (HidProtocol.CapabilitySpi | HidProtocol.CapabilitySdio)) == 0)
            throw new NotSupportedException("MSC control is unavailable on this firmware");
        var arguments = new byte[64];
        arguments[0] = force ? (byte)2 : enabled ? (byte)1 : (byte)0;
        SendCommand(3, force ? (byte)1 : (byte)0, arguments);
    }

    public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
    {
        EnsureCapability(HidProtocol.CapabilitySettings, "persistent settings");
        ValidateSettings(settings);
        var configuration = ReadConfiguration();
        var switches = (byte)(settings.Radios | ((configuration.BoardFlags & 32) != 0 ? 4 : 0));
        var arguments = new byte[64];
        BinaryPrimitives.WriteUInt16LittleEndian(arguments, (ushort)settings.SystemClockMhz);
        arguments[2] = (byte)settings.SdCardProfile;
        arguments[3] = switches;
        var result = SendCommand(5, force ? (byte)1 : (byte)0, arguments);
        var saved = new BootSettingsResponse(BinaryPrimitives.ReadUInt16LittleEndian(result.ResultData),
            result.ResultData[2], result.ResultData[3] & 3);
        if (saved != settings || result.ResultData[3] != switches || result.ResultData[4] != 0)
        {
            CommandOutcomeUncertain = true;
            throw new InvalidOperationException("Firmware did not confirm the selected next-boot settings");
        }
        return saved;
    }

    public void Reboot()
    {
        EnsureCapability(HidProtocol.CapabilityReboot, "reboot");
        SendCommand(2, 1, new byte[64]);
    }

    public WirelessStatusResponse ReadWireless()
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless provisioning");
        var configuration = ReadConfiguration();
        return HidProtocol.ParseWirelessStatus(
            readFeature(HidProtocol.WirelessReportId, HidProtocol.WirelessFeatureSize), configuration);
    }

    public byte[] TcpCliAuthProof(byte[] nonce, byte[] uid)
    {
        ArgumentNullException.ThrowIfNull(nonce);
        ArgumentNullException.ThrowIfNull(uid);
        EnsureCapability(HidProtocol.CapabilityWireless, "USB TCP CLI authentication");
        identity ??= ReadInfo();
        var expectedUid = Convert.FromHexString(identity.SerialNumber);
        if (uid.Length != expectedUid.Length || !CryptographicOperations.FixedTimeEquals(uid, expectedUid))
            throw new ArgumentException("TCP CLI challenge belongs to a different board");
        if (nonce.Length != 16) throw new ArgumentException("TCP CLI challenge must contain 16 bytes");
        var arguments = new byte[64];
        nonce.CopyTo(arguments, 0);
        return SendCommand(13, 0, arguments).ResultData[..16];
    }

    /// <summary>Stage only provided values and confirm the atomic save from the original opcode 5 result.</summary>
    public WirelessStatusResponse SetWireless(WirelessRequest settings)
    {
        if (CommandOutcomeUncertain)
            throw new InvalidOperationException("A prior command outcome is uncertain; further writes are blocked");
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless provisioning");
        EnsureCapability(HidProtocol.CapabilitySettings, "persistent settings");
        HidProtocol.ValidateWirelessRequest(settings);
        var previous = ReadWireless();
        if (previous.WifiState == "testing" || previous.ScanState == 1)
            throw new InvalidOperationException("A Wi-Fi test or scan is active");
        var ssid = settings.Ssid is not null || settings.SsidHex is not null
            ? HidProtocol.EncodeSsid(settings.Ssid, settings.SsidHex) : null;
        if (!settings.UseTestedWifi &&
                (previous.SsidStaged && ssid is null || previous.PasswordStaged && settings.Password is null) ||
            previous.KeyStaged && settings.BleKey is null)
            throw new InvalidOperationException("Discard or explicitly include the existing staged wireless values");
        if (settings.UseTestedWifi)
        {
            if (!previous.PairTested || (previous.StagedState & 8) != 0 ||
                ssid is not null && !ssid.SequenceEqual(Convert.FromHexString(previous.SsidHex)))
                throw new InvalidOperationException("The selected staged Wi-Fi pair has not passed testing");
        }
        else
        {
            if (ssid is not null) SendCommand(6, 0, HidProtocol.StagingArguments(6, ssid));
            if (settings.Password is not null)
                SendCommand(7, 0, HidProtocol.StagingArguments(7, Encoding.ASCII.GetBytes(settings.Password)));
        }
        if (settings.BleKey is not null)
            SendCommand(8, 0, HidProtocol.StagingArguments(8, HidProtocol.EncodeControlKey(settings.BleKey)));
        var staged = ReadWireless();
        if (settings.UseTestedWifi && !staged.PairTested ||
            !settings.UseTestedWifi && ssid is not null && !staged.SsidStaged ||
            settings.Password is not null && !staged.PasswordStaged ||
            settings.BleKey is not null && !staged.KeyStaged)
            throw new InvalidOperationException("Staged wireless values expired or were replaced; save was not sent");
        var commit = settings.UseTestedWifi || ssid is not null || settings.Password is not null ||
            settings.BleKey is not null;
        var switches = (byte)(settings.Board!.Radios |
            ((settings.TcpCliEnabled ?? previous.TcpCliEnabled) ? 4 : 0));
        var arguments = new byte[64];
        BinaryPrimitives.WriteUInt16LittleEndian(arguments, (ushort)settings.Board.SystemClockMhz);
        arguments[2] = (byte)settings.Board.SdCardProfile;
        arguments[3] = switches;
        var expectedMask = (staged.SsidStaged ? 1 : 0) | (staged.PasswordStaged ? 2 : 0) | (staged.KeyStaged ? 4 : 0);
        var result = SendCommand(5, commit ? (byte)2 : (byte)0, arguments);
        if (BinaryPrimitives.ReadUInt16LittleEndian(result.ResultData) != settings.Board.SystemClockMhz ||
            result.ResultData[2] != settings.Board.SdCardProfile || result.ResultData[3] != switches ||
            result.ResultData[4] != expectedMask)
        {
            CommandOutcomeUncertain = true;
            throw new InvalidOperationException("Saved settings were not confirmed; outcome is uncertain");
        }
        // Keep the last runtime snapshot, with only the verified saved configuration updated.
        return staged with
        {
            WifiEnabled = (result.ResultData[3] & 1) != 0,
            BleEnabled = (result.ResultData[3] & 2) != 0,
            TcpCliEnabled = (result.ResultData[3] & 4) != 0,
            BleKeyConfigured = settings.BleKey is null ? staged.BleKeyConfigured : settings.BleKey.Length != 0,
            NetworkConfigured = staged.SsidStaged ? staged.SsidHex.Length != 0 : staged.NetworkConfigured
        };
    }

    public IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null)
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless scan");
        var prior = ReadWireless();
        SendCommand(11, 0, new byte[64]);
        var generation = prior.ScanGeneration == ushort.MaxValue ? (ushort)1 : (ushort)(prior.ScanGeneration + 1);
        var result = WaitWireless(status => status.ScanState is 2 or 3, 15000, onPoll,
            status => status.ScanGeneration == generation);
        if (result.ScanState != 2) throw new InvalidOperationException("Wireless scan failed");
        var networks = new List<WirelessNetworkResponse>();
        for (byte index = 0; index < result.ScanCount; index++)
        {
            onPoll?.Invoke();
            var arguments = new byte[64];
            arguments[0] = index;
            BinaryPrimitives.WriteUInt16LittleEndian(arguments.AsSpan(1), result.ScanGeneration);
            var entry = SendCommand(12, 0, arguments);
            CommandOutcomeUncertain = true;
            networks.Add(HidProtocol.ParseScanEntry(entry, result.ScanGeneration, index));
            CommandOutcomeUncertain = false;
        }
        return networks;
    }

    public WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
        Action<string>? onProgress = null, string? ssidHex = null)
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless test");
        var encoded = HidProtocol.EncodeSsid(ssid, ssidHex);
        if (encoded.Length == 0) throw new ArgumentException("A Wi-Fi test requires a nonempty SSID");
        HidProtocol.ValidatePassword(password);
        SendCommand(6, 0, HidProtocol.StagingArguments(6, encoded));
        SendCommand(7, 0, HidProtocol.StagingArguments(7, Encoding.ASCII.GetBytes(password)));
        var prior = ReadWireless();
        SendCommand(9, 0, new byte[64]);
        var generation = prior.TestGeneration == byte.MaxValue ? (byte)1 : (byte)(prior.TestGeneration + 1);
        var result = WaitWireless(status => status.WifiState is "test_joined" or "test_failed", 35000, onPoll,
            status => status.TestGeneration == generation, onProgress);
        if (result.WifiState != "test_joined" || !result.PairTested)
            throw new InvalidOperationException($"Could not connect: {result.Phase}");
        return new(5, 5, result.Ipv4, result.Error, result.TestGeneration);
    }

    public void CancelWirelessTest()
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless test");
        SendCommand(10, 0, new byte[64]);
    }

    private WirelessStatusResponse WaitWireless(Func<WirelessStatusResponse, bool> terminal, int timeoutMilliseconds,
        Action? onPoll, Func<WirelessStatusResponse, bool> generationMatches, Action<string>? onProgress = null)
    {
        CommandOutcomeUncertain = true;
        var deadline = Environment.TickCount64 + timeoutMilliseconds;
        while (Environment.TickCount64 < deadline)
        {
            onPoll?.Invoke();
            var result = ReadWireless();
            if (generationMatches(result))
            {
                if (terminal(result))
                {
                    CommandOutcomeUncertain = false;
                    return result;
                }
                onProgress?.Invoke(result.Phase.Length == 0 ? "Joining network" : result.Phase);
            }
            Thread.Sleep(500);
        }
        throw new TimeoutException("Wireless operation timed out; do not resend until its result is checked");
    }

    public void SetSelfTest(string action, uint sizeMib)
    {
        EnsureCapability(HidProtocol.CapabilitySelfTest, "SD self-test");
        if (action is not ("start" or "cancel"))
            throw new ArgumentException("Self-test action must be start or cancel", nameof(action));
        if (action == "cancel") sizeMib = 0;
        else if (sizeMib is < 1 or > 1024)
            throw new ArgumentOutOfRangeException(nameof(sizeMib), "Self-test size must be 1..1024 MiB");
        var arguments = new byte[64];
        arguments[0] = action == "start" ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(arguments.AsSpan(1), sizeMib);
        SendCommand(4, 0, arguments);
    }

    public void Dispose() => stream?.Dispose();

    private byte[] GetFeatureReport(byte reportId, int requiredSize)
    {
        if (featureReport.Length != requiredSize)
            featureReport = new byte[requiredSize];
        Array.Clear(featureReport);
        featureReport[0] = reportId;
        stream.GetFeature(featureReport);
        return featureReport;
    }

    private ControlResult SendCommand(byte opcode, byte flags, byte[] arguments)
    {
        if (CommandOutcomeUncertain)
            throw new InvalidOperationException("A prior HID command outcome is uncertain; further writes are blocked");
        identity ??= ReadInfo();
        transactionId = transactionId == byte.MaxValue ? (byte)1 : (byte)(transactionId + 1);
        if (transactionId == 0) transactionId = 1;
        var request = HidProtocol.BuildControlRequest(transactionId, opcode, flags, arguments);
        var forcedSave = opcode == 5 && (flags & 1) != 0;
        CommandOutcomeUncertain = true;
        var disconnected = false;
        try { writeFeature(request); }
        catch (Exception error) when (forcedSave && error is IOException or TimeoutException)
        {
            // SET may have reached firmware before USB detached. Only read its original result from here.
            disconnected = true;
        }

        var retryRead = forcedSave || opcode == 13;
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
            catch (Exception error) when (retryRead && error is IOException or TimeoutException)
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
                var report = new byte[HidProtocol.IdentityFeatureSize];
                report[0] = HidProtocol.IdentityReportId;
                replacement.GetFeature(report);
                if (HidProtocol.ParseIdentityReport(report).SerialNumber != uid) continue;
                inputReport = new byte[Math.Max(
                    HidProtocol.SelfTestPayloadSize + 1, candidate.GetMaxInputReportLength())];
                featureReport = new byte[Math.Max(
                    HidProtocol.ControlFeatureSize, candidate.GetMaxFeatureReportLength())];
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
