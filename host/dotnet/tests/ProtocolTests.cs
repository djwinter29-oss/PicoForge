using System.Buffers.Binary;
using PicoForge.Host;
using Xunit;

namespace PicoForge.Host.Tests;

public sealed class ProtocolTests
{
    [Fact]
    public void ParsesProductMcuClockAndSerial()
    {
        var report = new byte[27];
        report[0] = HidProtocol.InfoReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 2;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(3), 150_000_000);
        "PicoForge"u8.CopyTo(report.AsSpan(7));

        Assert.Equal(new HardwareInfoResponse("PicoForge", "RP2350", 2, 150_000_000, "board-1"),
            HidProtocol.ParseInfoReport(report, "board-1"));
    }

    [Fact]
    public void ParsesRuntimeStateAndRejectsUnknownFlags()
    {
        var report = new byte[8];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 9;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(3), 123_456);
        report[7] = 3;

        Assert.Equal(new RuntimeStatusResponse(9, 123_456, true, true),
            HidProtocol.ParseStatusReport(report));

        report[7] = 0x80;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
    }

    [Fact]
    public void RejectsUnsupportedVersionAndClock()
    {
        var report = new byte[27];
        report[0] = HidProtocol.InfoReportId;
        report[1] = 2;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report, ""));

        report[1] = HidProtocol.ReportVersion;
        "PicoForge"u8.CopyTo(report.AsSpan(7));
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report, ""));
    }
}