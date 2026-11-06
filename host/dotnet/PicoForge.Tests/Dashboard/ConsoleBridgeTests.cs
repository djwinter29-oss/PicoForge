using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ConsoleBridgeTests
{
    [Fact]
    public async Task ShutdownAfterChallengeReceiveBeforeContinuationDoesNotStartProof()
    {
        await Task.Run(() =>
        {
            var (listener, port) = Listen();
            using var bridge = new ConsoleBridge(null, port, true);
            using var queued = new QueuedContext();
            var previous = SynchronizationContext.Current;
            Task<string>? opening = null;
            var proofs = 0;
            try
            {
                SynchronizationContext.SetSynchronizationContext(queued);
                opening = bridge.OpenAsync(Wireless("127.0.0.1"), (_, _) =>
                {
                    proofs++;
                    return new byte[16];
                });
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                using var peer = listener.AcceptSocketAsync(timeout.Token).AsTask().GetAwaiter().GetResult();
                peer.Send(Encoding.ASCII.GetBytes(
                    "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r"));
                while (queued.Callbacks.TryTake(out var callback, 100, timeout.Token))
                    callback.Callback(callback.State);
                Assert.False(opening.IsCompleted);
                peer.Send([(byte)'\n']);
                var received = queued.Callbacks.Take(timeout.Token);
                bridge.Dispose();
                received.Callback(received.State);
                while (!opening.IsCompleted)
                {
                    var callback = queued.Callbacks.Take(timeout.Token);
                    callback.Callback(callback.State);
                }
                Assert.IsType<BoardStateException>(Record.Exception(() => opening.GetAwaiter().GetResult()));
                Assert.Equal(0, proofs);
            }
            finally
            {
                SynchronizationContext.SetSynchronizationContext(previous);
                listener.Stop();
            }
        });
    }

    private sealed class QueuedContext : SynchronizationContext, IDisposable
    {
        public BlockingCollection<(SendOrPostCallback Callback, object? State)> Callbacks { get; } = new();
        public override void Post(SendOrPostCallback callback, object? state) => Callbacks.Add((callback, state));
        public void Dispose() => Callbacks.Dispose();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ShutdownDuringProofCannotPublishASessionOrHideAProofBug(bool proofBug)
    {
        var (listener, port) = Listen();
        using var proofEntered = new ManualResetEventSlim();
        using var releaseProof = new ManualResetEventSlim();
        using var bridge = new ConsoleBridge(null, port, true);
        Task<string>? opening = null;
        try
        {
            opening = bridge.OpenAsync(Wireless("127.0.0.1"), (_, _) =>
            {
                proofEntered.Set();
                Assert.True(releaseProof.Wait(TimeSpan.FromSeconds(5)));
                if (proofBug) throw new InvalidOperationException("proof bug");
                return new byte[16];
            });
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            Assert.True(proofEntered.Wait(TimeSpan.FromSeconds(2)));
            bridge.Dispose();
            bridge.Dispose();
            releaseProof.Set();
            if (proofBug) await Assert.ThrowsAsync<InvalidOperationException>(() => opening);
            else await Assert.ThrowsAsync<BoardStateException>(() => opening);
            await Assert.ThrowsAsync<BoardStateException>(() =>
                bridge.OpenAsync(Wireless("127.0.0.1"), (_, _) => new byte[16]));
        }
        finally
        {
            releaseProof.Set();
            listener.Stop();
            if (opening is not null) await Record.ExceptionAsync(() => opening);
        }
    }

    [Fact]
    public async Task AuthenticationBudgetIncludesSynchronousProofAndDoesNotSendALateResponse()
    {
        var (listener, port) = Listen();
        var time = new ManualTime();
        using var bridge = new ConsoleBridge(time, port, true);
        try
        {
            var opening = bridge.OpenAsync(Wireless("127.0.0.1"), (_, _) =>
            {
                time.Advance(TimeSpan.FromSeconds(9));
                return new byte[16];
            });
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            await Assert.ThrowsAsync<TimeoutException>(() => opening);
            var buffer = new byte[64];
            Assert.Equal(0, await peer.ReceiveAsync(buffer, SocketFlags.None, timeout.Token));
        }
        finally { listener.Stop(); }
    }

    [Fact]
    public async Task PendingOpenReservesTheOnlySessionAndShutdownClosesItsSocket()
    {
        var (listener, port) = Listen();
        using var bridge = new ConsoleBridge(null, port, true);
        try
        {
            var opening = bridge.OpenAsync(Wireless("127.0.0.1"), (_, _) => new byte[16]);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await Assert.ThrowsAsync<BoardStateException>(() =>
                bridge.OpenAsync(Wireless("127.0.0.1"), (_, _) => new byte[16]));
            bridge.Dispose();
            await Assert.ThrowsAsync<BoardStateException>(() => opening);
            try { Assert.Equal(0, await peer.ReceiveAsync(new byte[64], SocketFlags.None, timeout.Token)); }
            catch (SocketException error) when (error.SocketErrorCode is
                SocketError.ConnectionReset or SocketError.ConnectionAborted) { }
        }
        finally { listener.Stop(); }
    }

    private static WirelessStatusResponse Wireless(string address, bool keyConfigured = true) => new()
    {
        WifiEnabled = true,
        Ssid = "HomeDev",
        WifiState = WifiState.Connected,
        Ipv4 = address,
        BleState = BleState.Off,
        TcpCliStarted = true,
        TcpCliEnabled = true,
        NetworkConfigured = true,
        BleKeyConfigured = keyConfigured,
        SsidHex = "486F6D65446576"
    };

    private static (TcpListener Listener, int Port) Listen()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return (listener, ((IPEndPoint)listener.LocalEndpoint).Port);
    }

    [Fact]
    public async Task RelaysRawBytesToTheBoardPortAndReleasesTheSessionOnPeerClose()
    {
        var (listener, port) = Listen();
        try
        {
            using var bridge = new ConsoleBridge(null, port, true);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
            var opening = bridge.OpenAsync(Wireless(IPAddress.Loopback.ToString()), (_, _) => new byte[16]);
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            var token = await opening.WaitAsync(timeout.Token);
            var auth = new byte[64];
            var authCount = await peer.ReceiveAsync(auth, SocketFlags.None, timeout.Token);
            Assert.Equal("AUTH 00000000000000000000000000000000\r\n", Encoding.ASCII.GetString(auth[..authCount]));
            await Assert.ThrowsAsync<BoardStateException>(() =>
                bridge.OpenAsync(Wireless(IPAddress.Loopback.ToString()), null));
            Assert.Throws<BoardStateException>(() => bridge.Close("wrong"));
            using (var aborted = new CancellationTokenSource(TimeSpan.FromMilliseconds(100)))
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => bridge.ReadAsync(token, aborted.Token));
            Assert.Empty((await bridge.ReadAsync(token, timeout.Token)).Data);
            var command = Encoding.ASCII.GetBytes("help\r\x1b[A");
            bridge.Write(token, command);
            var buffer = new byte[1024];
            var count = await peer.ReceiveAsync(buffer, SocketFlags.None, timeout.Token);
            Assert.Equal(command, buffer[..count]);
            var output = Encoding.ASCII.GetBytes("PicoForge> \x1b[D<script>");
            await peer.SendAsync(output, SocketFlags.None, timeout.Token);
            var response = await bridge.ReadAsync(token, timeout.Token);
            Assert.Equal(output, response.Data);
            Assert.False(response.Closed);
            peer.Shutdown(SocketShutdown.Both);
            Assert.True((await bridge.ReadAsync(token, timeout.Token)).Closed);
            await Assert.ThrowsAsync<BoardStateException>(() => bridge.ReadAsync(token));
        }
        finally { listener.Stop(); }
    }

    [Fact]
    public async Task ExpiresAnIdleSessionOnTheInjectedClock()
    {
        var (listener, port) = Listen();
        try
        {
            var time = new ManualTime();
            using var bridge = new ConsoleBridge(time, port, true);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var opening = bridge.OpenAsync(Wireless(IPAddress.Loopback.ToString()), (_, _) => new byte[16]);
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            var token = await opening.WaitAsync(timeout.Token);
            time.Advance(TimeSpan.FromSeconds(15));
            bridge.Expire();
            await Assert.ThrowsAsync<BoardStateException>(() => bridge.ReadAsync(token));
        }
        finally { listener.Stop(); }
    }

    [Theory]
    [InlineData("127.0.0.1")]
    [InlineData("224.0.0.1")]
    [InlineData("0.0.0.0")]
    [InlineData("255.255.255.255")]
    [InlineData("bad")]
    [InlineData("::1")]
    public async Task DoesNotUseInvalidBoardAddressesAsProxyTargets(string address)
    {
        using var bridge = new ConsoleBridge(null, 2323, false);
        await Assert.ThrowsAsync<InvalidDataException>(() => bridge.OpenAsync(Wireless(address), null));
    }

    [Fact]
    public async Task DoesNotConnectWhenTheListenerHasNotStarted()
    {
        using var bridge = new ConsoleBridge();
        await Assert.ThrowsAsync<BoardStateException>(() =>
            bridge.OpenAsync(Wireless("192.168.66.87") with { TcpCliStarted = false }, null));
    }

    [Fact]
    public async Task AuthenticatesTheTcpCliWithTheSavedBleKey()
    {
        var (listener, port) = Listen();
        try
        {
            using var bridge = new ConsoleBridge(null, port, true);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var key = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
            var nonce = Enumerable.Range(0, 16).Select(value => (byte)value).ToArray();
            var uid = Enumerable.Range(0, 8).Select(value => (byte)value).ToArray();
            var opening = bridge.OpenAsync(Wireless(IPAddress.Loopback.ToString(), true), (challenge, boardUid) =>
                HMACSHA256.HashData(key, challenge.Concat(boardUid)
                    .Concat(Encoding.ASCII.GetBytes("PFTCPCLI1TCPCLI001")).ToArray()).AsSpan(0, 16).ToArray());
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                $"PicoForge AUTH {Convert.ToHexString(nonce).ToLowerInvariant()} " +
                $"{Convert.ToHexString(uid).ToLowerInvariant()}\r\n"), SocketFlags.None, timeout.Token);
            var token = await opening.WaitAsync(timeout.Token);
            var input = new byte[64];
            var count = await peer.ReceiveAsync(input, SocketFlags.None, timeout.Token);
            var message = nonce.Concat(uid).Concat(Encoding.ASCII.GetBytes("PFTCPCLI1TCPCLI001")).ToArray();
            var proof = HMACSHA256.HashData(key, message).AsSpan(0, 16).ToArray();
            Assert.Equal(Encoding.ASCII.GetBytes($"AUTH {Convert.ToHexString(proof).ToLowerInvariant()}\r\n"),
                input[..count]);
            bridge.Close(token);
        }
        finally { listener.Stop(); }
    }

    [Fact]
    public async Task RequiresUsbProofSupportWhenTheBoardRequiresTcpAuthentication()
    {
        using var bridge = new ConsoleBridge();
        await Assert.ThrowsAsync<BoardStateException>(() =>
            bridge.OpenAsync(Wireless("192.168.66.87", true)));
    }
}
