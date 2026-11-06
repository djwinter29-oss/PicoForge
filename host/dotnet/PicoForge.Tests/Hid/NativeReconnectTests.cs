using System.Buffers.Binary;
using HidSharp;
using Xunit;

namespace PicoForge.Tests;

public sealed class NativeReconnectTests
{
    [Theory]
    [InlineData("read-timeout")]
    [InlineData("write-timeout")]
    [InlineData("input-size")]
    [InlineData("feature-size")]
    [InlineData("read")]
    [InlineData("bug")]
    public void InitialSetupFailureClosesTheOpenedStream(string failure)
    {
        var device = new Device { SetupFailure = failure };
        if (failure is "read" or "bug") device.Stream.Identity = () => FailureReport(failure);
        if (failure == "bug") Assert.Throws<ArgumentException>(() => new PicoForgeHid(device, () => []));
        else Assert.Throws<IOException>(() => new PicoForgeHid(device, () => []));
        Assert.Equal(1, device.Opens);
        Assert.Equal(1, device.Stream.Closes);
    }

    [Theory]
    [InlineData("read-timeout")]
    [InlineData("write-timeout")]
    [InlineData("input-size")]
    [InlineData("feature-size")]
    [InlineData("read")]
    [InlineData("bug")]
    public async Task InitialSetupAndCloseFailureStopsTheMonitorWithoutOpeningAgain(string failure)
    {
        var device = new Device { SetupFailure = failure };
        if (failure is "read" or "bug") device.Stream.Identity = () => FailureReport(failure);
        device.Stream.CloseError = new IOException("initial close failed");
        var monitor = new DeviceMonitor(() => new PicoForgeHid(device, () => []));
        try
        {
            monitor.Start();
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(4));
            while (monitor.Snapshot.Error?.Contains("cleanup failed") != true)
                await Task.Delay(10, timeout.Token);
            Assert.False(monitor.Snapshot.Connected);
            Assert.Throws<BoardStateException>(() => monitor.ResyncCommand(true));
            var shutdown = await Assert.ThrowsAsync<IOException>(() => monitor.DisposeAsync().AsTask());
            var failedOpen = Assert.IsType<HidOpenCleanupException>(shutdown.InnerException);
            var causes = Assert.IsType<AggregateException>(failedOpen.InnerException);
            Assert.Equal(2, causes.InnerExceptions.Count);
            Assert.Equal("initial close failed", causes.InnerExceptions[1].Message);
            Assert.Throws<IOException>(() => failedOpen.Client.Dispose());
            Assert.Equal(1, device.Opens);
            Assert.Equal(1, device.Stream.Closes);
        }
        finally { await Assert.ThrowsAsync<IOException>(() => monitor.DisposeAsync().AsTask()); }
    }

    [Fact]
    public async Task FailedCandidateRemainsOwnedThroughMonitorShutdown()
    {
        var original = new Device();
        var candidate = new Device();
        candidate.Stream.Identity = () => FailureReport("read");
        candidate.Stream.CloseError = new IOException("candidate close failed");
        var hid = new PicoForgeHid(original, () => [candidate]);
        var created = 0;
        var monitor = new DeviceMonitor(() =>
        {
            Interlocked.Increment(ref created);
            return hid;
        });
        try
        {
            monitor.Start();
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            while (!monitor.Snapshot.Connected) await Task.Delay(10, timeout.Token);
            monitor.Attach(hid, monitor.Snapshot with
            {
                Memory = new MemoryStatusResponse(1, true, true, true, false, null, null, null, null, null, null)
                    { Msc = new(true, false, false) }
            });
            Assert.Throws<IOException>(() => monitor.SetSettings(new(200, 1, 0), true));
            while (monitor.Snapshot.Error?.Contains("cleanup failed") != true)
                await Task.Delay(10, timeout.Token);
            Assert.False(monitor.Snapshot.Connected);
            Assert.True(monitor.Snapshot.CommandOutcomeUncertain);
            await Assert.ThrowsAsync<IOException>(() => monitor.DisposeAsync().AsTask());
            Assert.Equal(1, created);
            Assert.Equal(1, candidate.Stream.Closes);
        }
        finally { await Assert.ThrowsAsync<IOException>(() => monitor.DisposeAsync().AsTask()); }
    }

    [Theory]
    [InlineData("read")]
    [InlineData("malformed")]
    [InlineData("identity")]
    [InlineData("bug")]
    public void CandidateCleanupFailureIsTerminalAndNeverOpensAnotherHandle(string failure)
    {
        var original = new Device();
        var candidate = new Device();
        var unused = new Device();
        candidate.Stream.Identity = () => FailureReport(failure);
        candidate.Stream.CloseError = new IOException("candidate close failed");
        var hid = new PicoForgeHid(original, () => [candidate, unused]);

        var error = Assert.Throws<IOException>(() => hid.SetSettings(new(200, 1, 0), true));
        Assert.Equal("candidate close failed", error.Message);
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Equal(1, original.Stream.Writes);
        Assert.Equal(1, original.Stream.Closes);
        Assert.Equal(1, candidate.Stream.Closes);
        Assert.Equal(0, unused.Opens);
        Assert.Contains("cleanup failed", Assert.Throws<IOException>(() => hid.ReadConfiguration()).Message);
        Assert.Throws<IOException>(() => hid.SetSettings(new(200, 1, 0), true));
        Assert.Throws<IOException>(() => hid.Dispose());
        Assert.Throws<IOException>(() => hid.Dispose());
        Assert.Equal(1, candidate.Stream.Closes);
        Assert.Equal(0, unused.Opens);
    }

    [Theory]
    [InlineData("open")]
    [InlineData("read")]
    [InlineData("malformed")]
    [InlineData("identity")]
    public void InvalidCandidatesAreClosedBeforeAdoptingTheOriginalIdentity(string failure)
    {
        var original = new Device();
        var rejected = new Device();
        var valid = new Device();
        if (failure == "open") rejected.OpenError = new IOException("not ready");
        else rejected.Stream.Identity = () => FailureReport(failure);
        using var hid = new PicoForgeHid(original, () => [rejected, valid]);
        valid.Stream.DetachOnWrite = false;

        Assert.Equal(new(200, 1, 0), hid.SetSettings(new(200, 1, 0), true));
        Assert.False(hid.CommandOutcomeUncertain);
        Assert.Equal(1, original.Stream.Writes);
        Assert.Equal(1, original.Stream.Closes);
        Assert.Equal(failure == "open" ? 0 : 1, rejected.Stream.Closes);
        Assert.Equal(1, valid.Opens);
        Assert.Equal(0, valid.Stream.Closes);
        Assert.Equal(0, valid.Stream.Writes);
        hid.Dispose();
        Assert.Equal(1, valid.Stream.Closes);
    }

    [Fact]
    public void OriginalCloseFailureNeverOpensAReplacementOrRetriesClose()
    {
        var original = new Device();
        var candidate = new Device();
        var hid = new PicoForgeHid(original, () => [candidate]);
        original.Stream.CloseError = new IOException("original close failed");
        Assert.Equal("original close failed",
            Assert.Throws<IOException>(() => hid.SetSettings(new(200, 1, 0), true)).Message);
        Assert.True(hid.CommandOutcomeUncertain);
        Assert.Throws<IOException>(() => hid.Dispose());
        Assert.Equal(1, original.Stream.Closes);
        Assert.Equal(0, candidate.Opens);
    }

    [Fact]
    public void UnexpectedValidationBugPropagatesAfterSuccessfulCleanup()
    {
        var original = new Device();
        var candidate = new Device();
        candidate.Stream.Identity = () => FailureReport("bug");
        using var hid = new PicoForgeHid(original, () => [candidate]);
        Assert.Equal("identity bug",
            Assert.Throws<ArgumentException>(() => hid.SetSettings(new(200, 1, 0), true)).Message);
        Assert.Equal(1, candidate.Stream.Closes);
        Assert.True(hid.CommandOutcomeUncertain);
    }

    private static byte[] FailureReport(string failure)
    {
        if (failure == "read") throw new IOException("identity unavailable");
        if (failure == "bug") throw new ArgumentException("identity bug");
        var report = IdentityReport();
        if (failure == "malformed") report[1] = 255;
        if (failure == "identity") report[4] = 255;
        return report;
    }

    private static byte[] IdentityReport()
    {
        var report = new byte[HidProtocol.IdentityFeatureSize];
        report[0] = HidProtocol.IdentityReportId;
        report[1] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(report.AsSpan(2), 2);
        for (var index = 0; index < 8; index++) report[index + 4] = (byte)index;
        report[15] = report[16] = 2;
        report[18] = HidProtocol.CapabilityLed | HidProtocol.CapabilitySettings | HidProtocol.CapabilitySdio;
        BinaryPrimitives.WriteUInt32LittleEndian(report.AsSpan(19), 520192);
        return report;
    }

    /// <summary>A device whose real HidSharp Open path returns a controlled native-stream substitute.</summary>
    private sealed class Device : HidDevice
    {
        public NativeStream Stream { get; }
        public string? SetupFailure { get; init; }
        public Exception? OpenError { get; set; }
        public int Opens { get; private set; }
        public Device() => Stream = new(this);
        public override int ProductID => HidProtocol.ProductId;
        public override int VendorID => HidProtocol.VendorId;
        public override int ReleaseNumberBcd => 0;
        public override string DevicePath => "/fake/hid";
        public override string GetFileSystemName() => DevicePath;
        public override int GetMaxInputReportLength() => SetupFailure == "input-size"
            ? throw new IOException("input size unavailable") : HidProtocol.SelfTestPayloadSize + 1;
        public override int GetMaxOutputReportLength() => 0;
        public override int GetMaxFeatureReportLength() => SetupFailure == "feature-size"
            ? throw new IOException("feature size unavailable") : HidProtocol.ControlFeatureSize;
        protected override DeviceStream OpenDeviceDirectly(OpenConfiguration configuration)
        {
            Opens++;
            if (OpenError is not null) throw OpenError;
            return Stream;
        }
    }

    /// <summary>Counts native writes and closes, including failed closes that must not be retried.</summary>
    private sealed class NativeStream(Device device) : HidStream(device)
    {
        public Func<byte[]> Identity { get; set; } = IdentityReport;
        public Exception? CloseError { get; set; }
        public bool DetachOnWrite { get; set; } = true;
        public int Writes { get; private set; }
        public int Closes { get; private set; }
        public override int ReadTimeout
        {
            get;
            set
            {
                if (device.SetupFailure == "read-timeout") throw new IOException("read timeout unavailable");
                field = value;
            }
        }
        public override int WriteTimeout
        {
            get;
            set
            {
                if (device.SetupFailure == "write-timeout") throw new IOException("write timeout unavailable");
                field = value;
            }
        }
        public override void GetFeature(byte[] buffer, int offset, int count)
        {
            byte[] report;
            if (buffer[offset] == HidProtocol.IdentityReportId) report = Identity();
            else if (buffer[offset] == HidProtocol.ConfigurationReportId)
                report = ProtocolTests.ConfigurationReport();
            else
            {
                report = new byte[HidProtocol.ControlFeatureSize];
                report[0] = HidProtocol.ControlReportId;
                report[1] = report[2] = 1;
                report[3] = (byte)Opcode.SaveSettings;
                report[4] = (byte)ControlStatus.Completed;
                report[6] = 200;
                report[8] = 1;
            }
            report.CopyTo(buffer, offset);
        }
        public override void SetFeature(byte[] buffer, int offset, int count)
        {
            Writes++;
            if (DetachOnWrite) throw new IOException("detached after SET");
        }
        public override int Read(byte[] buffer, int offset, int count) => 0;
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        protected override void Dispose(bool disposing)
        {
            if (!disposing) return;
            Closes++;
            if (CloseError is not null) throw CloseError;
            base.Dispose(disposing);
        }
    }
}
