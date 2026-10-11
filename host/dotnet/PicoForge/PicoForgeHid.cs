using HidSharp;

namespace PicoForge;

internal sealed class PicoForgeHid : IDisposable
{
    private readonly HidDevice device;
    private readonly HidStream stream;

    public PicoForgeHid()
    {
        var candidates = DeviceList.Local.GetHidDevices(HidProtocol.VendorId, HidProtocol.ProductId)
            .Where(HasPicoForgeUsage).ToArray();
        if (candidates.Length == 0)
            throw new InvalidOperationException("PicoForge HID interface not found; check USB connection and HID permissions");
        if (candidates.Length != 1)
            throw new InvalidOperationException("Multiple PicoForge HID interfaces matched; connect one board at a time");

        device = candidates[0];
        stream = device.Open();
        stream.ReadTimeout = 250;
        stream.WriteTimeout = 1000;
    }

    public HardwareInfoResponse ReadInfo()
    {
        var report = new byte[Math.Max(HidProtocol.InfoFeatureSize, device.GetMaxFeatureReportLength() + 1)];
        report[0] = HidProtocol.InfoReportId;
        stream.GetFeature(report);
        return HidProtocol.ParseInfoReport(report);
    }

    public RuntimeStatusResponse? ReadStatus()
    {
        var report = new byte[Math.Max(HidProtocol.StatusInputSize, device.GetMaxInputReportLength())];
        try
        {
            var count = stream.Read(report, 0, report.Length);
            return count == 0 ? null : HidProtocol.ParseStatusReport(report.AsSpan(0, count));
        }
        catch (TimeoutException)
        {
            return null;
        }
    }

    public void SetLed(bool on)
    {
        stream.SetFeature(HidProtocol.BuildLedCommandReport(on));
    }

    public void Dispose() => stream.Dispose();

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