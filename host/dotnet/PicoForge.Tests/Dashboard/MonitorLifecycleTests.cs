using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;

namespace PicoForge.Tests;

public sealed class MonitorLifecycleTests
{
    [Fact]
    public async Task ProductionStatusContractIncludesRegistryConflicts()
    {
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        using var monitor = new TestMonitor { Snapshot = new(true, null, CommandOutcomeUncertain: true) };
        using var boards = new DashboardBoards(null, null, () => attached, (_, _) => monitor);
        var id = boards.Boards().Boards[0].Id;
        boards.Monitor(id);
        attached = [new("/hid/a", "board-A", "PicoForge"), new("/hid/b", "board-A", "PicoForge")];
        Assert.Empty(boards.Boards().Boards);

        var builder = WebApplication.CreateBuilder(new WebApplicationOptions { EnvironmentName = "Development" });
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton<IDashboardBoards>(boards);
        await using var app = await DashboardApplication.BuildAsync(builder);
        await app.StartAsync();
        using var http = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        using var conflict = await http.GetAsync($"/api/status?board={id}");
        Assert.Equal(HttpStatusCode.Conflict, conflict.StatusCode);
        Assert.Contains("ambiguous", (await conflict.Content.ReadFromJsonAsync<JsonElement>())
            .GetProperty("error").GetString());
        var document = await http.GetFromJsonAsync<JsonElement>("/openapi/v1.json");
        var responses = document.GetProperty("paths").GetProperty("/api/status")
            .GetProperty("get").GetProperty("responses");
        Assert.True(responses.TryGetProperty("200", out _));
        foreach (var code in new[] { "404", "409" })
            Assert.Contains("ApiError", responses.GetProperty(code).GetProperty("content")
                .GetProperty("application/json").GetProperty("schema").GetProperty("$ref").GetString());
        await app.StopAsync();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AcceptedSaveOrRebootBlocksAllFurtherWritesUntilNewConnection(bool reboot)
    {
        var client = new Client();
        await using var monitor = new DeviceMonitor(() => client);
        var ready = new StatusResponse(true, null, client.ReadInfo() with { Capabilities = 0x8f },
            new(1, 0, 200_000_000, true, false, false, null, true, false),
            Memory: new MemoryStatusResponse(1, true, true, true, false, null, null, null, null, null, null)
            { Msc = new(false, false, false) }, BoardConfiguration: client.ReadConfiguration());
        monitor.Attach(client, ready);
        if (reboot) monitor.Reboot();
        else monitor.SetSettings(new(200, 1, 0));
        Assert.True(monitor.Snapshot.RebootPending);
        Action[] commands =
        [
            () => monitor.SetLed(true),
            () => monitor.SetMsc(true),
            () => monitor.SetSettings(new(250, 1, 0)),
            () => monitor.SetSelfTest("start", 1),
            () => monitor.SetSelfTest("cancel", 1),
            () => monitor.Reboot(),
            () => monitor.ScanWireless(),
            () => monitor.TestWireless("Pico", "password"),
            () => monitor.CancelWirelessTest(),
            () => monitor.SetWireless(new()),
            () => monitor.TcpCliAuthProof(new byte[16], new byte[8])
        ];
        foreach (var command in commands)
            Assert.Equal("Board reboot is pending; wait for reconnection",
                Assert.Throws<BoardStateException>(command).Message);
        Assert.Throws<BoardStateException>(() => monitor.SetMsc(false, force: true));
        Assert.Equal(1, client.Writes);
        Assert.False(monitor.ResyncCommand(true).Uncertain);
        monitor.Attach(client, ready);
        Assert.False(monitor.Snapshot.RebootPending);
        monitor.SetLed(true);
        Assert.Equal(2, client.Writes);
    }

    [Theory]
    [InlineData("io")]
    [InlineData("protocol")]
    [InlineData("permission")]
    [InlineData("timeout")]
    public async Task ConfigurationFailureKeepsIdentityAndTelemetryButBlocksDependentCommands(string failure)
    {
        var client = new Client
        {
            Capabilities = 0x89,
            Configuration = () => throw (failure switch
            {
                "io" => new IOException("Feature 2 unavailable"),
                "protocol" => new InvalidDataException("Feature 2 invalid"),
                "permission" => new UnauthorizedAccessException("Feature 2 denied"),
                _ => new TimeoutException("Feature 2 timed out")
            })
        };
        await using var monitor = new DeviceMonitor(() => client);
        monitor.Start();
        await WaitUntil(() => monitor.Snapshot.Runtime is not null);
        var status = monitor.Snapshot;
        Assert.True(status.Connected);
        Assert.NotNull(status.Hardware);
        Assert.Null(status.Error);
        Assert.Null(status.BoardConfiguration);
        Assert.False(status.SettingsSupported);
        Assert.StartsWith("Boot settings unavailable:", status.SettingsMessage);
        Assert.Equal(status.SettingsMessage, status.WirelessError);
        Assert.Null(status.Wireless);
        Assert.True(client.Reads > 0);
        foreach (var command in new Action[]
        {
            () => monitor.SetSettings(new(200, 1, 0)), () => monitor.SetMsc(true),
            () => monitor.Reboot(), () => monitor.SetSelfTest("start", 1),
            () => monitor.ScanWireless(), () => monitor.CancelWirelessTest(),
            () => monitor.TcpCliAuthProof(new byte[16], new byte[8])
        })
            Assert.Throws<BoardStateException>(command);
        Assert.Equal(0, client.Writes);
        monitor.SetLed(true);
        Assert.Equal(1, client.Writes);
        Assert.False(monitor.ResyncCommand(true).Uncertain);
    }

    [Fact]
    public async Task ReconnectionRecoversConfigurationWithoutLosingDegradedDiagnostics()
    {
        var disconnect = 0;
        var degraded = new Client
        {
            Configuration = () => throw new IOException("Feature 2 unavailable"),
            OnRead = () =>
            {
                if (Volatile.Read(ref disconnect) != 0) throw new IOException("unplugged");
            }
        };
        var healthy = new Client();
        var attempts = 0;
        await using var monitor = new DeviceMonitor(() =>
            Interlocked.Increment(ref attempts) == 1 ? degraded : healthy);
        monitor.Start();
        await WaitUntil(() => monitor.Snapshot.Runtime is not null);
        Assert.False(monitor.Snapshot.SettingsSupported);
        Assert.Equal(1, Volatile.Read(ref attempts));
        Volatile.Write(ref disconnect, 1);
        await WaitUntil(() => monitor.Snapshot.SettingsSupported && monitor.Snapshot.Runtime is not null);
        Assert.Null(monitor.Snapshot.SettingsMessage);
        Assert.Null(monitor.Snapshot.WirelessError);
        Assert.Equal(1, degraded.Closes);
        Assert.Equal(2, attempts);
    }

    [Theory]
    [InlineData(Opcode.Reboot, ControlStatus.Completed, true)]
    [InlineData(Opcode.SaveSettings, ControlStatus.Completed, true)]
    [InlineData(Opcode.Led, ControlStatus.Completed, false)]
    [InlineData(Opcode.SaveSettings, ControlStatus.Failed, false)]
    internal void ReconciledRebootingCommandsRetainTheWriteGate(Opcode opcode, ControlStatus status, bool pending)
    {
        var client = new Client
        {
            Reconciled = new(new(1, opcode, status, ControlError.None, new byte[48]), null)
        };
        using var monitor = new DeviceMonitor();
        monitor.Attach(client, new(true, null, CommandOutcomeUncertain: true,
            BoardConfiguration: client.ReadConfiguration()));
        Assert.False(monitor.ResyncCommand(true).Uncertain);
        Assert.Equal(pending, monitor.Snapshot.RebootPending);
        if (pending) Assert.Throws<BoardStateException>(() => monitor.SetSelfTest("start", 1));
        Assert.Equal(0, client.Writes);
    }

    [Fact]
    public async Task UnexpectedConfigurationBugIsNotTreatedAsReadOnlyAvailability()
    {
        var client = new Client { Configuration = () => throw new ArgumentException("configuration parser bug") };
        await using var monitor = new DeviceMonitor(() => client);
        monitor.Start();
        await WaitUntil(() => monitor.Snapshot.Error?.StartsWith("Internal monitor error") == true);
        Assert.False(monitor.Snapshot.Connected);
        Assert.Null(monitor.Snapshot.SettingsMessage);
        Assert.Equal(0, client.Reads);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task RetirementCannotPassAnActiveCommandOrLoseItsUncertainty(bool uncertain, bool ambiguous)
    {
        var clock = new ManualTime();
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var client = new Client();
        client.OnLed = () =>
        {
            entered.Set();
            Assert.True(release.Wait(TimeSpan.FromSeconds(5)));
            if (uncertain)
            {
                client.CommandOutcomeUncertain = true;
                throw new TimeoutException("late uncertain write");
            }
        };
        var created = 0;
        var monitor = new DeviceMonitor(() => client);
        await using var boards = new DashboardBoards(null, null, () => attached, (_, _) =>
        {
            Interlocked.Increment(ref created);
            return monitor;
        }, clock);
        var id = boards.Boards().Boards[0].Id;
        boards.Monitor(id);
        await WaitUntil(() => monitor.Snapshot.Runtime is not null);
        var command = Task.Run(() => Record.Exception(() => monitor.SetLed(true)));
        try
        {
            Assert.True(entered.Wait(TimeSpan.FromSeconds(2)));
            attached = ambiguous
                ? [new("/hid/a", "board-A", "PicoForge"), new("/hid/b", "board-A", "PicoForge")]
                : [];
            clock.Advance(TimeSpan.FromSeconds(31));
            await Task.Run(boards.Boards).WaitAsync(TimeSpan.FromSeconds(2));
            if (ambiguous) Assert.Throws<BoardStateException>(() => boards.Monitor(id));
            else Assert.Same(monitor, boards.Monitor(id));
            Assert.Equal(0, client.Closes);
            release.Set();
            var error = await command.WaitAsync(TimeSpan.FromSeconds(2));
            if (ambiguous)
            {
                Assert.Throws<BoardStateException>(() => boards.Monitor(id));
                Assert.Contains("ambiguous", Assert.Throws<BoardStateException>(() => monitor.SetLed(false)).Message);
                attached = [];
                boards.Boards();
                if (uncertain)
                    Assert.Contains("ambiguous",
                        Assert.Throws<BoardStateException>(() => monitor.SetLed(false)).Message);
            }
            if (uncertain)
            {
                Assert.IsType<TimeoutException>(error);
                boards.Boards();
                attached = [new("/hid/new", "board-A", "PicoForge")];
                boards.Boards();
                Assert.Same(monitor, boards.Monitor(id));
                Assert.True(monitor.Snapshot.CommandOutcomeUncertain);
                Assert.Throws<BoardStateException>(() => monitor.SetLed(false));
                Assert.Equal(1, created);
                Assert.Equal(1, client.Writes);
            }

            else
            {
                Assert.Null(error);
                await WaitUntil(() =>
                {
                    boards.Boards();
                    try { boards.Monitor(id); return false; }
                    catch (BoardNotFoundException) { return true; }
                });
                Assert.Throws<BoardStateException>(() => monitor.SetLed(false));
            }
        }
        finally
        {
            release.Set();
            await command;
        }
    }

    [Fact]
    public async Task UniqueScanClearsQuarantineWithoutRetiringAnActiveOwner()
    {
        HidBoard[] attached = [new("/hid/a", "board-A", "PicoForge")];
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var client = new Client { OnLed = () =>
        {
            entered.Set();
            Assert.True(release.Wait(TimeSpan.FromSeconds(5)));
        } };
        var monitor = new DeviceMonitor(() => client);
        await using var boards = new DashboardBoards(null, null, () => attached, (_, _) => monitor);
        var id = boards.Boards().Boards[0].Id;
        boards.Monitor(id);
        await WaitUntil(() => monitor.Snapshot.Runtime is not null);
        var command = Task.Run(() => monitor.SetLed(true));
        try
        {
            Assert.True(entered.Wait(TimeSpan.FromSeconds(2)));
            attached = [new("/hid/a", "board-A", "PicoForge"), new("/hid/b", "board-A", "PicoForge")];
            Assert.Empty(boards.Boards().Boards);
            Assert.Throws<BoardStateException>(() => boards.Monitor(id));
            attached = [new("/hid/a", "board-A", "PicoForge")];
            Assert.Single(boards.Boards().Boards);
            Assert.Same(monitor, boards.Monitor(id));
        }
        finally { release.Set(); await command; }
        monitor.SetLed(false);
        Assert.Equal(2, client.Writes);
        Assert.Equal(0, client.Closes);
    }

    [Theory]
    [InlineData("poll")]
    [InlineData("stale")]
    [InlineData("disconnected")]
    [InlineData("missing")]
    public async Task ConsoleHttpRejectsUnavailableWirelessStatus(string failure)
    {
        var clock = new ManualTime();
        var failPoll = 0;
        var wireless = new WirelessStatusResponse
        {
            WifiEnabled = true,
            WifiState = WifiState.Connected,
            Ipv4 = "127.0.0.1",
            TcpCliEnabled = true,
            TcpCliStarted = true,
            BleKeyConfigured = true
        };
        var client = new Client
        {
            Capabilities = 0x89,
            Wireless = () =>
            Volatile.Read(ref failPoll) != 0 ? throw new IOException("Feature 12 unavailable") : wireless
        };
        await using var monitor = new DeviceMonitor(() => client, time: clock);
        if (failure == "poll")
        {
            monitor.Start();
            await WaitUntil(() => monitor.Snapshot.Wireless is not null);
            Volatile.Write(ref failPoll, 1);
            clock.Advance(TimeSpan.FromSeconds(2));
            await WaitUntil(() => monitor.Snapshot.WirelessError == "Feature 12 unavailable");
            Assert.Same(wireless, monitor.Snapshot.Wireless);
        }
        else
        {
            monitor.Attach(client, new(failure != "disconnected", null, client.ReadInfo(),
                BoardConfiguration: client.ReadConfiguration(), Wireless: failure == "missing" ? null : wireless));
            if (failure == "stale") clock.Advance(TimeSpan.FromSeconds(5));
        }
        using var listener = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        listener.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        listener.Listen(1);
        using var bridge = new ConsoleBridge(null, ((IPEndPoint)listener.LocalEndPoint!).Port, true);
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton<IDashboardBoards>(new OneBoard(monitor));
        builder.Services.AddSingleton(bridge);
        await using var app = await DashboardApplication.BuildAsync(builder);
        await app.StartAsync();
        using var http = new HttpClient { BaseAddress = new Uri(app.Urls.Single()) };
        using var response = await http.PostAsJsonAsync("/api/console?board=board-a", new { });
        Assert.Equal(HttpStatusCode.Conflict, response.StatusCode);
        Assert.False(string.IsNullOrEmpty((await response.Content.ReadFromJsonAsync<JsonElement>())
            .GetProperty("error").GetString()));
        Assert.Equal(0, client.Writes);
        Assert.False(listener.Poll(0, SelectMode.SelectRead));
        if (failure == "poll")
        {
            Volatile.Write(ref failPoll, 0);
            clock.Advance(TimeSpan.FromSeconds(2));
            await WaitUntil(() => monitor.Snapshot.WirelessError is null);
            Assert.Same(wireless, monitor.Snapshot.Wireless);
            var opening = http.PostAsJsonAsync("/api/console?board=board-a", new { });
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            using var peer = await listener.AcceptAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            using var connected = await opening.WaitAsync(timeout.Token);
            Assert.Equal(HttpStatusCode.OK, connected.StatusCode);
            Assert.Equal(64, (await connected.Content.ReadFromJsonAsync<JsonElement>())
                .GetProperty("session").GetString()!.Length);
            Assert.Equal(1, client.Writes);
        }
        await app.StopAsync();
    }

    [Fact]
    public async Task RetirementRejectsPreviouslyObtainedReferencesIncludingNoOpResync()
    {
        var client = new Client();
        await using var monitor = new DeviceMonitor(() => client);
        monitor.Attach(client, new(true, null));
        Assert.True(monitor.TryRetire());
        Assert.Throws<BoardStateException>(() => monitor.ResyncCommand(true));
        Assert.Throws<BoardStateException>(() => monitor.SetLed(true));
        Assert.Equal(0, client.Writes);
    }

    [Fact]
    public async Task QuarantineBlocksEveryWriteButRetainsReadOnlyReconciliation()
    {
        var client = new Client { Reconciled = new(new(1, Opcode.Led, ControlStatus.Completed,
            ControlError.None, new byte[48]), null) };
        await using var monitor = new DeviceMonitor(() => client);
        monitor.Attach(client, new(true, null, client.ReadInfo(), CommandOutcomeUncertain: true,
            BoardConfiguration: client.ReadConfiguration()));
        monitor.SetQuarantined(true);
        foreach (var command in new Action[]
        {
            () => monitor.SetLed(true), () => monitor.SetMsc(true), () => monitor.SetSettings(new(200, 1, 0)),
            () => monitor.SetSelfTest("start", 1), () => monitor.Reboot(), () => monitor.ScanWireless(),
            () => monitor.TestWireless("Pico", "password"), () => monitor.CancelWirelessTest(),
            () => monitor.SetWireless(new()), () => monitor.TcpCliAuthProof(new byte[16], new byte[8])
        })
            Assert.Contains("ambiguous", Assert.Throws<BoardStateException>(command).Message);
        Assert.False(monitor.ResyncCommand(true).Uncertain);
        Assert.Contains("ambiguous", Assert.Throws<BoardStateException>(() => monitor.SetLed(false)).Message);
        Assert.Equal(0, client.Writes);
    }

    [Fact]
    public async Task RegistryKeepsRetiringOwnershipUntilCloseFinishesAndShutdownJoinsIt()
    {
        using var closing = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var client = new Client { OnClose = () =>
        {
            closing.Set();
            Assert.True(release.Wait(TimeSpan.FromSeconds(5)));
        } };
        HidBoard[] attached = [new("/hid/a", null, "PicoForge")];
        var monitor = new DeviceMonitor(() => client);
        var boards = new DashboardBoards(null, null, () => attached, (_, _) => monitor);
        var id = boards.Boards().Boards[0].Id;
        boards.Monitor(id);
        await WaitUntil(() => monitor.Snapshot.Connected);
        try
        {
            attached = [];
            await WaitUntil(() =>
            {
                boards.Boards();
                return closing.IsSet;
            });
            attached = [new("/hid/a", null, "PicoForge")];
            boards.Boards();
            Assert.Throws<BoardStateException>(() => boards.Monitor(id));
            var stopping = boards.DisposeAsync().AsTask();
            Assert.False(stopping.IsCompleted);
            release.Set();
            await stopping.WaitAsync(TimeSpan.FromSeconds(2));
            await boards.DisposeAsync();
            Assert.Equal(1, client.Closes);
        }
        finally
        {
            release.Set();
            await boards.DisposeAsync();
        }
    }

    [Fact]
    public async Task CleanupFailureIsVisibleRetainedAndDoesNotPreventOtherBoardsFromStopping()
    {
        var failed = new Client { OnRead = () => throw new IOException("read failed"),
            OnClose = () => throw new IOException("native cleanup failed") };
        var healthy = new Client();
        var failedMonitor = new DeviceMonitor(() => failed);
        var healthyMonitor = new DeviceMonitor(() => healthy);
        HidBoard[] attached = [new("/a", "A", "PicoForge"), new("/b", "B", "PicoForge")];
        var boards = new DashboardBoards(null, null, () => attached,
            (serial, _) => serial == "A" ? failedMonitor : healthyMonitor);
        var list = boards.Boards();
        boards.Monitor(list.Boards[0].Id);
        boards.Monitor(list.Boards[1].Id);
        await WaitUntil(() => failedMonitor.Snapshot.Error?.Contains("cleanup failed") == true);
        await WaitUntil(() => healthyMonitor.Snapshot.Connected);
        Assert.False(failedMonitor.Snapshot.Connected);
        await Assert.ThrowsAsync<IOException>(() => boards.DisposeAsync().AsTask());
        await Assert.ThrowsAsync<IOException>(() => boards.DisposeAsync().AsTask());
        Assert.Equal(1, failed.Closes);
        Assert.Equal(1, healthy.Closes);
        Assert.Throws<BoardStateException>(() => failedMonitor.ResyncCommand(true));
    }

    [Fact]
    public async Task ShutdownRefusesQueuedCommandsButWaitsForAnAcceptedCommand()
    {
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var client = new Client { OnLed = () =>
        {
            entered.Set();
            Assert.True(release.Wait(TimeSpan.FromSeconds(5)));
        } };
        var monitor = new DeviceMonitor(() => client);
        monitor.Start();
        await WaitUntil(() => monitor.Snapshot.Runtime is not null);
        var active = Task.Run(() => monitor.SetLed(true));
        Assert.True(entered.Wait(TimeSpan.FromSeconds(2)));
        var queued = Task.Run(() => Record.Exception(() => monitor.SetLed(false)));
        try
        {
            var stopped = monitor.DisposeAsync().AsTask();
            Assert.False(stopped.IsCompleted);
            release.Set();
            await active.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.IsType<BoardStateException>(await queued.WaitAsync(TimeSpan.FromSeconds(2)));
            await stopped.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Equal(1, client.Writes);
            Assert.Equal(1, client.Closes);
        }
        finally
        {
            release.Set();
            await active;
            await queued;
            await monitor.DisposeAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MonitorCapacityIncludesRetiringAndFailedOwners(bool fail)
    {
        HidBoard[] attached = Enumerable.Range(0, 17)
            .Select(i => new HidBoard($"/{i}", null, "PicoForge")).ToArray();
        var closing = new ClosingMonitor();
        var boards = new DashboardBoards(null, null, () => attached,
            (_, path) => path == "/0" ? closing : new TestMonitor());
        var list = boards.Boards();
        foreach (var board in list.Boards.Take(16)) boards.Monitor(board.Id);
        Assert.Throws<BoardStateException>(() => boards.Monitor(list.Boards[16].Id));
        attached = attached.Skip(1).ToArray();
        boards.Boards();
        Assert.True(closing.Started.Task.IsCompleted);
        Assert.Throws<BoardStateException>(() => boards.Monitor(list.Boards[16].Id));
        if (fail)
        {
            closing.Finished.SetException(new IOException("cleanup failed"));
            await Assert.ThrowsAsync<IOException>(() => boards.DisposeAsync().AsTask());
        }
        else
        {
            closing.Finished.SetResult();
            await WaitUntil(() =>
            {
                boards.Boards();
                try { boards.Monitor(list.Boards[16].Id); return true; }
                catch (BoardStateException) { return false; }
            });
            await boards.DisposeAsync();
        }
    }

    private static async Task WaitUntil(Func<bool> ready)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        while (!ready()) await Task.Delay(10, timeout.Token);
    }

    private sealed class ClosingMonitor : TestMonitor
    {
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Finished { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override async ValueTask DisposeAsync()
        {
            Started.TrySetResult();
            await Finished.Task;
            Dispose();
        }
    }

    private sealed class Client : IHidClient
    {
        public Action? OnLed { get; set; }
        public Action? OnRead { get; init; }
        public Action? OnClose { get; init; }
        public Func<BoardConfigurationResponse>? Configuration { get; init; }
        public Func<WirelessStatusResponse>? Wireless { get; init; }
        public CommandResyncResult? Reconciled { get; init; }
        public byte Capabilities { get; init; } = 9;
        public int Writes;
        public int Closes;
        public int Reads;
        public bool CommandOutcomeUncertain { get; set; }
        public ControlCommandReference? UncertainCommand => new(1, Opcode.Led);
        public WirelessOperationReference? UncertainWirelessOperation => null;
        public ControlResult? LastCommandResult { get; private set; }
        public HardwareInfoResponse ReadInfo() => new("PicoForge", "RP2350", 2, "0001020304050607",
            Capabilities: Capabilities);
        public BoardConfigurationResponse ReadConfiguration() =>
            Configuration is { } read ? read() : new(200, 1, 0, 0, 0, [], 0, 0, 0, 0);
        public InputReport? ReadInputReport(int timeoutMilliseconds = 250)
        {
            OnRead?.Invoke();
            Interlocked.Increment(ref Reads);
            return new(new(1, 0, 200_000_000, true, false, false, null, true, false), null);
        }
        public void SetLed(bool on)
        {
            Interlocked.Increment(ref Writes);
            OnLed?.Invoke();
        }
        public void Dispose()
        {
            Interlocked.Increment(ref Closes);
            OnClose?.Invoke();
        }
        public void SetMsc(bool enabled, bool force = false) => throw new NotSupportedException();
        public BootSettingsResponse SetSettings(BootSettingsResponse settings, bool force = false)
        {
            Interlocked.Increment(ref Writes);
            LastCommandResult = new(1, Opcode.SaveSettings, ControlStatus.Completed, ControlError.None, []);
            return settings;
        }
        public void Reboot()
        {
            Interlocked.Increment(ref Writes);
            LastCommandResult = new(1, Opcode.Reboot, ControlStatus.Completed, ControlError.None, []);
        }
        public void SetSelfTest(string action, uint sizeMib) => throw new NotSupportedException();
        public WirelessStatusResponse ReadWireless() => Wireless?.Invoke() ?? throw new NotSupportedException();
        public byte[] TcpCliAuthProof(byte[] nonce, byte[] uid)
        {
            Interlocked.Increment(ref Writes);
            return new byte[16];
        }
        public CommandResyncResult? ReconcileOutcome(ControlCommandReference? command,
            WirelessOperationReference? operation) => Reconciled ?? throw new NotSupportedException();
        public WirelessStatusResponse SetWireless(WirelessRequest settings) => throw new NotSupportedException();
        public IReadOnlyList<WirelessNetworkResponse> ScanWireless(Action? onPoll = null) =>
            throw new NotSupportedException();
        public WirelessOperationResponse TestWireless(string ssid, string password, Action? onPoll = null,
            Action<string>? onProgress = null, string? ssidHex = null, CancellationToken cancel = default) =>
            throw new NotSupportedException();
        public void CancelWirelessTest() => throw new NotSupportedException();
    }
}
