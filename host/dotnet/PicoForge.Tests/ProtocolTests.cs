using System.Buffers.Binary;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

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
    public void ParsesPaddedStandardFeatureReportWhenVersionEqualsReportId()
    {
        var report = new byte[28];
        report[0] = HidProtocol.InfoReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 2;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(3), 150_000_000);
        "PicoForge"u8.CopyTo(report.AsSpan(7));

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report, "board-1").ProductName);
    }

    [Fact]
    public void ParsesZeroPrefixedFeatureReport()
    {
        var report = new byte[28];
        report[0] = 0;
        report[1] = HidProtocol.InfoReportId;
        report[2] = HidProtocol.ReportVersion;
        report[3] = 2;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(4), 150_000_000);
        "PicoForge"u8.CopyTo(report.AsSpan(8));

        Assert.Equal("PicoForge", HidProtocol.ParseInfoReport(report, "board-1").ProductName);
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
    public void ParsesBoardTemperatureAndLedState()
    {
        var report = new byte[] { HidProtocol.BoardStatusReportId, 1, 7, 0x29, 0x09 };

        Assert.Equal(new BoardStatusResponse(true, 2345, true, true),
            HidProtocol.ParseBoardStatusReport(report));
    }

    [Fact]
    public void ParsesZeroPrefixedBoardFeatureAndBuildsLedCommands()
    {
        var report = new byte[] { 0, HidProtocol.BoardStatusReportId, 1, 1, 0, 0 };

        Assert.Equal(new BoardStatusResponse(true, 0, false, false),
            HidProtocol.ParseBoardStatusReport(report));
        Assert.Equal(new byte[] { HidProtocol.LedCommandReportId, 1, 1 },
            HidProtocol.BuildLedCommandReport(true));
        Assert.Equal(new byte[] { HidProtocol.LedCommandReportId, 1, 0 },
            HidProtocol.BuildLedCommandReport(false));
    }

    [Fact]
    public void RejectsBoardStatusWithLedOnButUnavailable()
    {
        var report = new byte[] { HidProtocol.BoardStatusReportId, 1, 5, 0, 0 };

        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseBoardStatusReport(report));
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