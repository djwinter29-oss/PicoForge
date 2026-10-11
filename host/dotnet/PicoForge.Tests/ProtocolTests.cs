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
    public void ParsesMemoryReportAndSdCid()
    {
        var report = CreateMemoryReport(0x3f, 1, 2, 25_000_000, 4096,
            [0x1b, (byte)'S', (byte)'D', (byte)'C', (byte)'A', (byte)'R', (byte)'D', (byte)'1',
             0x21, 0x12, 0x34, 0x56, 0x78, 0x01, 0xaa, 0xff]);

        Assert.Equal(new MemoryStatusResponse(7, true, true, true, true, "sd-card", "spi",
            25_000_000, 4096, 2_097_152,
            new SdCardIdentity(0x1b, "SD", "CARD1", "2.1", 0x12345678, 2026, 10)),
            HidProtocol.ParseMemoryReport(report));
    }

    [Fact]
    public void ParsesUninitializedMemoryWithUnknownPresence()
    {
        var report = CreateMemoryReport(0x01, 1, 1, 0, 0, new byte[16]);

        var memory = HidProtocol.ParseMemoryReport(report);
        Assert.True(memory.Configured);
        Assert.False(memory.PresenceKnown);
        Assert.Null(memory.Present);
        Assert.Null(memory.CapacityBytes);
        Assert.Null(memory.Manufacturer);
    }

    [Fact]
    public void RejectsInitializedMemoryWithoutKnownPresence()
    {
        var report = CreateMemoryReport(0x3d, 1, 2, 25_000_000, 4096, new byte[16]);

        Assert.Throws<InvalidOperationException>(() => HidProtocol.ParseMemoryReport(report));
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

    private static byte[] CreateMemoryReport(byte flags, byte component, byte mode,
        uint busClockHz, uint blocks, byte[] cid)
    {
        var report = new byte[HidProtocol.MemoryPayloadSize + 1];
        report[0] = HidProtocol.MemoryReportId;
        report[1] = HidProtocol.ReportVersion;
        report[2] = 7;
        report[3] = flags;
        report[4] = component;
        report[5] = mode;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(6), busClockHz);
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(10), blocks);
        cid.CopyTo(report.AsSpan(14));
        return report;
    }
}