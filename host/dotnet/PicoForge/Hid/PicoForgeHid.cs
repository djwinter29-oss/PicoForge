using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using HidSharp;

namespace PicoForge;

public sealed record HidBoard(string DevicePath, string? SerialNumber, string ProductName);

internal sealed class PicoForgeHid : IHidClient
{
    private HidDevice device = null!;
    private HidStream stream = null!;
    private readonly Func<byte, int, byte[]> readFeature;
    private readonly Action<byte[]> writeFeature;
    private readonly Action<string> reopen;
    private readonly Func<IEnumerable<HidDevice>> matchingDevices;
    private Exception? cleanupError;
    private bool disposed;
    private byte[] inputReport;
    private byte[] featureReport;
    private readonly TimeProvider time;
    private HardwareInfoResponse identity;
    // ponytail: Feature 2 changes only through opcode 5 or a reboot (a new handle), so it is read once per handle
    // and again after a save. Another host saving over WebHID meanwhile leaves Wi-Fi switch display stale until then.
    private BoardConfigurationResponse? configuration;
    private byte transactionId;
    public bool CommandOutcomeUncertain { get; private set; }
    public ControlCommandReference? UncertainCommand { get; private set; }
    public WirelessOperationReference? UncertainWirelessOperation { get; private set; }

    public PicoForgeHid(string? serialNumber = null, string? devicePath = null)
        : this(SelectDevice(serialNumber, devicePath), MatchingDevices) { }

    private static HidDevice SelectDevice(string? serialNumber, string? devicePath)
    {
        if (serialNumber is not null && devicePath is not null)
            throw new ArgumentException("--serial and --device-path cannot be used together");
        var candidates = MatchingDevices()
            .Where(candidate => devicePath is null || candidate.DevicePath == devicePath)
            .Where(candidate => serialNumber is null || candidate.GetSerialNumber() == serialNumber)
            .ToArray();
        if (candidates.Length == 0)
            throw new IOException("PicoForge HID interface not found; check USB connection and HID permissions");
        if (candidates.Length != 1)
            throw new BoardStateException(
                "Multiple PicoForge HID interfaces matched; connect one board at a time");

        return candidates[0];
    }

    internal PicoForgeHid(HidDevice device, Func<IEnumerable<HidDevice>> matchingDevices)
    {
        this.device = device;
        this.matchingDevices = matchingDevices;
        stream = device.Open();
        stream.ReadTimeout = 250;
        stream.WriteTimeout = 1000;
        inputReport = new byte[Math.Max(HidProtocol.SelfTestPayloadSize + 1, device.GetMaxInputReportLength())];
        featureReport = new byte[Math.Max(
            HidProtocol.ControlFeatureSize, device.GetMaxFeatureReportLength())];
        readFeature = GetFeatureReport;
        writeFeature = report => { RequireOpen(); stream.SetFeature(report); };
        reopen = ReopenSameBoard;
        time = TimeProvider.System;
        try { identity = ReadHardwareInfo(); }
        catch
        {
            Dispose();
            throw;
        }
    }

    internal PicoForgeHid(HardwareInfoResponse identity, Func<byte, int, byte[]> readFeature,
        Action<byte[]> writeFeature, Action<string> reopen, TimeProvider? time = null)
    {
        this.identity = identity;
        this.time = time ?? TimeProvider.System;
        this.readFeature = readFeature;
        this.writeFeature = writeFeature;
        this.reopen = reopen;
        matchingDevices = MatchingDevices;
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

    public HardwareInfoResponse ReadInfo() => identity;

    private HardwareInfoResponse ReadHardwareInfo() =>
        HidProtocol.ParseIdentityReport(readFeature(HidProtocol.IdentityReportId, HidProtocol.IdentityFeatureSize));

    public BoardConfigurationResponse ReadConfiguration()
    {
        return configuration = HidProtocol.ParseConfigurationReport(
            readFeature(HidProtocol.ConfigurationReportId, HidProtocol.ConfigurationFeatureSize), identity);
    }

    public InputReport? ReadInputReport(int timeoutMilliseconds = 250)
    {
        RequireOpen();
        ArgumentOutOfRangeException.ThrowIfNegative(timeoutMilliseconds);
        var previousTimeout = stream.ReadTimeout;
        stream.ReadTimeout = timeoutMilliseconds;
        try
        {
            var count = stream.Read(inputReport, 0, inputReport.Length);
            if (count == 0) return null;
            var data = inputReport.AsSpan(0, count);
            return data[0] switch
            {
                HidProtocol.TelemetryReportId => new(HidProtocol.ParseTelemetryReport(data), null, null),
                HidProtocol.StorageReportId => HidProtocol.ParseStorageReport(data, identity),
                HidProtocol.SelfTestReportId when (identity.Capabilities & HidProtocol.CapabilitySelfTest) != 0
                    => new(null, null, HidProtocol.ParseSelfTestReport(data)),
                HidProtocol.SelfTestReportId => throw new InvalidDataException(
                    "firmware published self-test status without advertising the capability"),
                _ => throw new InvalidDataException($"unexpected HID input report ID {data[0]}")
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
        SendCommand(Opcode.Led, arguments: arguments);
    }

    public void SetMsc(bool enabled, bool force = false)
    {
        if (force && enabled) throw new BoardStateException("Force return requires disabling MSC");
        if ((identity.Capabilities & (HidProtocol.CapabilitySpi | HidProtocol.CapabilitySdio)) == 0)
            throw new BoardStateException("MSC control is unavailable on this firmware");
        var arguments = new byte[64];
        arguments[0] = force ? (byte)2 : enabled ? (byte)1 : (byte)0;
        SendCommand(Opcode.Msc, force ? (byte)1 : (byte)0, arguments);
    }

    public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
    {
        EnsureCapability(HidProtocol.CapabilitySettings, "persistent settings");
        var arguments = HidProtocol.EncodeSettings(settings, Configuration().TcpCliEnabled);
        SaveSettings(arguments, force ? HidProtocol.SaveForceMsc : (byte)0, 0,
            "Firmware did not confirm the selected next-boot settings");
        return settings;
    }

    /// <summary>Sends opcode 5; a result that does not echo the arguments and mask is an uncertain outcome.</summary>
    private ControlResult SaveSettings(byte[] arguments, byte flags, int mask, string unconfirmed)
    {
        var result = SendCommand(Opcode.SaveSettings, flags, arguments);
        configuration = null;
        if (HidProtocol.SettingsConfirmed(result, arguments, mask)) return result;
        CommandOutcomeUncertain = true;
        UncertainCommand = new(result.TransactionId, result.Opcode);
        throw new InvalidDataException(unconfirmed);
    }

    private BoardConfigurationResponse Configuration() => configuration ?? ReadConfiguration();

    public void Reboot()
    {
        EnsureCapability(HidProtocol.CapabilityReboot, "reboot");
        SendCommand(Opcode.Reboot, 1);
    }

    public WirelessStatusResponse ReadWireless()
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless provisioning");
        var saved = Configuration();
        return HidProtocol.ParseWirelessStatus(
            readFeature(HidProtocol.WirelessReportId, HidProtocol.WirelessFeatureSize), saved);
    }

    public byte[] TcpCliAuthProof(byte[] nonce, byte[] uid)
    {
        ArgumentNullException.ThrowIfNull(nonce);
        ArgumentNullException.ThrowIfNull(uid);
        EnsureCapability(HidProtocol.CapabilityWireless, "USB TCP CLI authentication");
        var expectedUid = Convert.FromHexString(identity.SerialNumber);
        if (uid.Length != expectedUid.Length || !CryptographicOperations.FixedTimeEquals(uid, expectedUid))
            throw new InvalidDataException("TCP CLI challenge belongs to a different board");
        if (nonce.Length != 16) throw new InvalidDataException("TCP CLI challenge must contain 16 bytes");
        var arguments = new byte[64];
        nonce.CopyTo(arguments, 0);
        return SendCommand(Opcode.TcpProof, arguments: arguments).ResultData[..16];
    }

    /// <summary>
    /// Stage only provided values and confirm the atomic save from the original opcode 5 result.
    /// <paramref name="settings"/> must come from <see cref="WirelessRequest.Parse"/>.
    /// </summary>
    public WirelessStatusResponse SetWireless(WirelessRequest settings)
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless provisioning");
        EnsureCapability(HidProtocol.CapabilitySettings, "persistent settings");
        var previous = ReadWireless();
        if (previous.WifiState == WifiState.Testing ||
            (WirelessScanState)previous.ScanState == WirelessScanState.Scanning)
            throw new BoardStateException("A Wi-Fi test or scan is active");
        var ssid = settings.Ssid is not null || settings.SsidHex is not null
            ? HidProtocol.EncodeSsid(settings.Ssid, settings.SsidHex) : null;
        if (!settings.UseTestedWifi &&
                (previous.SsidStaged && ssid is null || previous.PasswordStaged && settings.Password is null) ||
            previous.KeyStaged && settings.BleKey is null)
            throw new BoardStateException("Discard or explicitly include the existing staged wireless values");
        if (settings.UseTestedWifi)
        {
            if (!previous.PairTested ||
                ((WirelessStagingFlags)previous.StagedState & WirelessStagingFlags.PairTested) != 0 ||
                ssid is not null && !ssid.SequenceEqual(Convert.FromHexString(previous.SsidHex)))
                throw new BoardStateException("The selected staged Wi-Fi pair has not passed testing");
        }
        else
        {
            if (ssid is not null) Stage(Opcode.StageSsid, ssid);
            if (settings.Password is not null) Stage(Opcode.StagePassword, Encoding.ASCII.GetBytes(settings.Password));
        }
        if (settings.BleKey is not null) Stage(Opcode.StageKey, HidProtocol.EncodeControlKey(settings.BleKey));
        var staged = ReadWireless();
        if (settings.UseTestedWifi && !staged.PairTested ||
            !settings.UseTestedWifi && ssid is not null && !staged.SsidStaged ||
            settings.Password is not null && !staged.PasswordStaged ||
            settings.BleKey is not null && !staged.KeyStaged)
            throw new BoardStateException("Staged wireless values expired or were replaced; save was not sent");
        var commit = settings.UseTestedWifi || ssid is not null || settings.Password is not null ||
            settings.BleKey is not null;
        var arguments = HidProtocol.EncodeSettings(settings.Board!, settings.TcpCliEnabled ?? previous.TcpCliEnabled);
        var expectedMask = (staged.SsidStaged ? 1 : 0) | (staged.PasswordStaged ? 2 : 0) | (staged.KeyStaged ? 4 : 0);
        var result = SaveSettings(arguments, commit ? HidProtocol.SaveCommitWireless : (byte)0, expectedMask,
            "Saved settings were not confirmed; outcome is uncertain");
        // Keep the last runtime snapshot, with only the verified saved configuration updated.
        return staged with
        {
            WifiEnabled = (result.ResultData[3] & 1) != 0,
            BleEnabled = (result.ResultData[3] & 2) != 0,
            TcpCliEnabled = (result.ResultData[3] & HidProtocol.SwitchTcpCli) != 0,
            BleKeyConfigured = settings.BleKey is null ? staged.BleKeyConfigured : settings.BleKey.Length != 0,
            NetworkConfigured = staged.SsidStaged ? staged.SsidHex.Length != 0 : staged.NetworkConfigured
        };
    }

    public IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null)
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless scan");
        var prior = ReadWireless();
        SendCommand(Opcode.WifiScan);
        var generation = prior.ScanGeneration == ushort.MaxValue ? (ushort)1 : (ushort)(prior.ScanGeneration + 1);
        var result = WaitWireless(status =>
                (WirelessScanState)status.ScanState is WirelessScanState.Complete or WirelessScanState.Failed,
            15000, onPoll, "scan", generation);
        if ((WirelessScanState)result.ScanState != WirelessScanState.Complete)
            throw new BoardStateException("Wireless scan failed");
        var networks = new List<WirelessNetworkResponse>();
        for (byte index = 0; index < result.ScanCount; index++)
        {
            onPoll?.Invoke();
            var arguments = new byte[64];
            arguments[0] = index;
            BinaryPrimitives.WriteUInt16LittleEndian(arguments.AsSpan(1), result.ScanGeneration);
            var entry = SendCommand(Opcode.WifiEntry, arguments: arguments);
            networks.Add(HidProtocol.ParseScanEntry(entry, result.ScanGeneration, index));
        }
        return networks;
    }

    /// <summary>Stages and tests a Wi-Fi pair; the endpoint has already validated the SSID and password.</summary>
    public WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
        Action<string>? onProgress = null, string? ssidHex = null, CancellationToken cancel = default)
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless test");
        Stage(Opcode.StageSsid, HidProtocol.EncodeSsid(ssid, ssidHex));
        Stage(Opcode.StagePassword, Encoding.ASCII.GetBytes(password));
        var prior = ReadWireless();
        SendCommand(Opcode.WifiTest);
        var generation = prior.TestGeneration == byte.MaxValue ? (byte)1 : (byte)(prior.TestGeneration + 1);
        var result = WaitWireless(status =>
                status.WifiState is WifiState.TestJoined or WifiState.TestFailed, 35000, onPoll,
            "test", generation, onProgress, cancel);
        if (result.WifiState != WifiState.TestJoined || !result.PairTested)
            throw new BoardStateException($"Could not connect: {result.Phase}");
        return new(5, 5, result.Ipv4, result.Error, result.TestGeneration);
    }

    public void CancelWirelessTest()
    {
        EnsureCapability(HidProtocol.CapabilityWireless, "wireless test");
        SendCommand(Opcode.WifiCancel);
    }

    public CommandResyncResult? ReconcileOutcome(ControlCommandReference? command,
        WirelessOperationReference? wirelessOperation)
    {
        if (command is null && wirelessOperation is null) return null;
        ControlResult? controlResult = null;
        if (command is { } reference)
        {
            controlResult = HidProtocol.ParseControlResult(
                readFeature(HidProtocol.ControlReportId, HidProtocol.ControlFeatureSize));
            if (controlResult.TransactionId != reference.TransactionId || controlResult.Opcode != reference.Opcode ||
                controlResult.Status is ControlStatus.Idle or ControlStatus.Pending)
                return null;
        }
        WirelessStatusResponse? wireless = null;
        if (wirelessOperation is { } operation)
        {
            wireless = ReadWireless();
            var finished = operation.Kind switch
            {
                "scan" => wireless.ScanGeneration == operation.Generation &&
                    (WirelessScanState)wireless.ScanState is WirelessScanState.Complete or WirelessScanState.Failed,
                "test" => wireless.TestGeneration == operation.Generation &&
                    wireless.WifiState is WifiState.TestJoined or WifiState.TestFailed,
                _ => false
            };
            if (!finished) return null;
        }
        CommandOutcomeUncertain = false;
        UncertainCommand = null;
        UncertainWirelessOperation = null;
        return new(controlResult, wireless);
    }

    private void Stage(Opcode opcode, byte[] value) =>
        SendCommand(opcode, arguments: HidProtocol.StagingArguments(opcode, value));

    private WirelessStatusResponse WaitWireless(Func<WirelessStatusResponse, bool> terminal, int timeoutMilliseconds,
        Action? onPoll, string kind, ushort expectedGeneration, Action<string>? onProgress = null,
        CancellationToken cancel = default)
    {
        CommandOutcomeUncertain = true;
        UncertainWirelessOperation = new(kind, expectedGeneration);
        var started = time.GetTimestamp();
        while (time.GetElapsedTime(started).TotalMilliseconds < timeoutMilliseconds)
        {
            if (cancel.IsCancellationRequested)
            {
                // Cancel is a new verified command that drops the test, so the test's outcome is known again.
                CommandOutcomeUncertain = false;
                UncertainWirelessOperation = null;
                CancelWirelessTest();
                throw new WirelessTestCancelledException();
            }
            onPoll?.Invoke();
            var result = ReadWireless();
            var generationMatches = kind == "scan"
                ? result.ScanGeneration == expectedGeneration
                : result.TestGeneration == expectedGeneration;
            if (generationMatches)
            {
                if (terminal(result))
                {
                    CommandOutcomeUncertain = false;
                    UncertainWirelessOperation = null;
                    return result;
                }
                onProgress?.Invoke(result.Phase.Length == 0 ? "Joining network" : result.Phase);
            }
            cancel.WaitHandle.WaitOne(500);
        }
        throw new TimeoutException("Wireless operation timed out; do not resend until its result is checked");
    }

    /// <summary>Starts (<c>"start"</c>) or cancels the benchmark; the endpoint validated action and size.</summary>
    public void SetSelfTest(string action, uint sizeMib)
    {
        EnsureCapability(HidProtocol.CapabilitySelfTest, "SD self-test");
        var start = action == "start";
        var arguments = new byte[64];
        arguments[0] = start ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(arguments.AsSpan(1), start ? sizeMib : 0);
        SendCommand(Opcode.SelfTest, arguments: arguments);
    }

    public void Dispose()
    {
        ThrowIfCleanupFailed();
        if (disposed) return;
        try { stream?.Dispose(); }
        catch (Exception error)
        {
            cleanupError = error;
            throw;
        }
        disposed = true;
    }

    private void ThrowIfCleanupFailed()
    {
        if (cleanupError is not null)
            throw new IOException("HID cleanup failed; restart the dashboard before reconnecting", cleanupError);
    }

    private void RequireOpen()
    {
        ThrowIfCleanupFailed();
        ObjectDisposedException.ThrowIf(disposed, this);
    }

    private byte[] GetFeatureReport(byte reportId, int requiredSize)
    {
        RequireOpen();
        if (featureReport.Length != requiredSize)
            featureReport = new byte[requiredSize];
        Array.Clear(featureReport);
        featureReport[0] = reportId;
        stream.GetFeature(featureReport);
        return featureReport;
    }

    private ControlResult SendCommand(Opcode opcode, byte flags = 0, byte[]? arguments = null)
    {
        RequireOpen();
        if (CommandOutcomeUncertain)
            throw new BoardStateException("A prior HID command outcome is uncertain; further writes are blocked");
        transactionId = transactionId == byte.MaxValue ? (byte)1 : (byte)(transactionId + 1);
        var request = HidProtocol.BuildControlRequest(transactionId, opcode, flags, arguments ?? new byte[64]);
        var forcedSave = opcode == Opcode.SaveSettings && (flags & HidProtocol.SaveForceMsc) != 0;
        CommandOutcomeUncertain = true;
        UncertainCommand = new(transactionId, opcode);
        var disconnected = false;
        try { writeFeature(request); }
        catch (Exception error) when (forcedSave && error is IOException or TimeoutException)
        {
            // SET may have reached firmware before USB detached. Only read its original result from here.
            disconnected = true;
        }

        var retryRead = forcedSave || opcode == Opcode.TcpProof;
        var started = time.GetTimestamp();
        // ponytail: 20 ms real sleeps pace the result poll; only the deadline follows the TimeProvider.
        while (time.GetElapsedTime(started).TotalMilliseconds < (forcedSave ? 5000 : 1000))
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
            catch (Exception error) when (retryRead && cleanupError is null && error is IOException or TimeoutException)
            {
                disconnected = true;
                Thread.Sleep(20);
                continue;
            }
            var result = HidProtocol.ParseControlResult(response);
            if (result.TransactionId != transactionId || result.Opcode != opcode)
                throw new InvalidDataException("HID control result was replaced by another transaction");
            if (result.Status != ControlStatus.Pending)
            {
                CommandOutcomeUncertain = false;
                UncertainCommand = null;
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
        Dispose();
        foreach (var candidate in matchingDevices())
        {
            var adopted = false;
            try
            {
                stream = candidate.Open();
                disposed = false;
                stream.ReadTimeout = 250;
                stream.WriteTimeout = 1000;
                var report = new byte[HidProtocol.IdentityFeatureSize];
                report[0] = HidProtocol.IdentityReportId;
                stream.GetFeature(report);
                if (HidProtocol.ParseIdentityReport(report).SerialNumber != uid) continue;
                inputReport = new byte[Math.Max(
                    HidProtocol.SelfTestPayloadSize + 1, candidate.GetMaxInputReportLength())];
                featureReport = new byte[Math.Max(
                    HidProtocol.ControlFeatureSize, candidate.GetMaxFeatureReportLength())];
                device = candidate;
                adopted = true;
                return;
            }
            catch (Exception error) when (error is IOException or TimeoutException or InvalidDataException
                or InvalidOperationException)
            {
                // A board may still be reconnecting; do not select an unverified identity.
            }
            finally { if (!adopted) Dispose(); }
        }
        throw new IOException("Waiting for the original PicoForge board UID to reconnect");
    }

    private void EnsureCapability(byte capability, string operation)
    {
        if ((identity.Capabilities & capability) == 0)
            throw new BoardStateException($"{operation} is unavailable on this firmware");
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

/// <summary>A Wi-Fi test stopped by a cancel request; the cancel command completed, so the outcome is known.</summary>
internal sealed class WirelessTestCancelledException() : InvalidOperationException("Wi-Fi test cancelled");
