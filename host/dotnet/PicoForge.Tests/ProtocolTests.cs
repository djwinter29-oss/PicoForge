using System.Buffers.Binary;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ProtocolTests
{
    [Fact]
    public void ParsesProductMcuAndSerialIdentity()
    {
        var report = CreateInfoReport();

        Assert.Equal(new HardwareInfoResponse("PicoForge", "RP2350", 2, "0123456789abcdef"),
            HidProtocol.ParseInfoReport(report));
    }

    [Fact]
    public void ParsesPaddedStandardFeatureReportWhenVersionEqualsReportId()
    {
        var report = new byte[44];
        CreateInfoReport().CopyTo(report, 0);

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report).ProductName);
    }

    [Fact]
    public void ParsesZeroPrefixedFeatureReport()
    {
        var report = new byte[45];
        report[0] = 0;
        report[1] = HidProtocol.InfoReportId;
        CreateInfoReport().AsSpan(1).CopyTo(report.AsSpan(2));

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report).ProductName);
    }

    [Fact]
    public void ParsesHidSharpFeatureReportWithRepeatedReportIdPrefix()
    {
        var report = new byte[44];
        report[0] = HidProtocol.InfoReportId;
        report[1] = HidProtocol.InfoReportId;
        CreateInfoReport().AsSpan(1).CopyTo(report.AsSpan(2));

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report).ProductName);
    }

    [Fact]
    public void ParsesRuntimeStateAndRejectsUnknownFlags()
    {
        var report = new byte[14];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 9;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(3), 123_456);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 150_000_000);
        report[11] = 0x1f;
        BinaryPrimitives.WriteInt16LittleEndian(report.AsSpan(12), 2345);

        var runtime = HidProtocol.ParseStatusReport(report);
        Assert.Equal(new RuntimeStatusResponse(9, 123_456, 150_000_000, true, true,
            true, 2345, true, true), runtime);
        Assert.Equal(new BoardStatusResponse(true, 2345, true, true), runtime.ToBoardStatus());

        report[11] = 0x80;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
    }

    [Fact]
    public void RejectsDynamicLedOnStatusWhenLedUnavailable()
    {
        var report = new byte[14];
        report[0] = HidProtocol.StatusReportId;
        report[1] = HidProtocol.ReportVersion;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(7), 125_000_000);
        report[11] = 0x10;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseStatusReport(report));
        Assert.Equal(new byte[] { HidProtocol.LedCommandReportId, 2, 1 },
            HidProtocol.BuildLedCommandReport(true));
        Assert.Equal(new byte[] { HidProtocol.LedCommandReportId, 2, 0 },
            HidProtocol.BuildLedCommandReport(false));
    }

    [Fact]
    public void RejectsUnsupportedVersionAndReservedIdentityBytes()
    {
        var report = CreateInfoReport();
        report[1] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report));

        report[1] = HidProtocol.ReportVersion;
        report[^1] = 1;
        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseInfoReport(report));
    }

    private static byte[] CreateInfoReport()
    {
        var report = new byte[43];
        report[0] = HidProtocol.InfoReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 2;
        "PicoForge"u8.CopyTo(report.AsSpan(3));
        "0123456789abcdef"u8.CopyTo(report.AsSpan(23));
        return report;
    }
}