using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ConsoleBridgeTests
{
    private static WirelessStatusResponse Wireless(string address, bool authRequired = true) =>
        new(true, false, "HomeDev", "connected", address, 0, "off", true, true, true, authRequired,
            "486F6D65446576", false, false, false, false, 0, 0, 0, 0, 0);

    [Fact]
    public async Task RelaysRawBytesToTheBoardPortAndReleasesTheSessionOnPeerClose()
    {
        // Use a local non-loopback interface as the simulated board, not physical hardware.
        var address = Dns.GetHostAddresses(Dns.GetHostName()).First(ip =>
            ip.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(ip));
        var listener = new TcpListener(address, 2323);
        listener.Start();
        try
        {
            using var bridge = new ConsoleBridge();
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var opening = Task.Run(() => bridge.Open(Wireless(address.ToString()), (_, _) => new byte[16]));
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            var token = await opening.WaitAsync(timeout.Token);
            var auth = new byte[64];
            var authCount = await peer.ReceiveAsync(auth, SocketFlags.None, timeout.Token);
            Assert.Equal("AUTH 00000000000000000000000000000000\r\n", Encoding.ASCII.GetString(auth[..authCount]));
            Assert.Throws<InvalidOperationException>(() => bridge.Open(Wireless(address.ToString()), null));
            Assert.Throws<InvalidOperationException>(() => bridge.Close("wrong"));
            var command = Encoding.ASCII.GetBytes("help\r\x1b[A");
            bridge.Write(token, command);
            var buffer = new byte[1024];
            var count = await peer.ReceiveAsync(buffer, SocketFlags.None, timeout.Token);
            Assert.Equal(command, buffer[..count]);
            var output = Encoding.ASCII.GetBytes("PicoForge> \x1b[D<script>");
            await peer.SendAsync(output, SocketFlags.None, timeout.Token);
            var response = bridge.Read(token);
            Assert.Equal(output, response.Data);
            Assert.False(response.Closed);
            Assert.Empty(bridge.Read(token).Data);
            peer.Shutdown(SocketShutdown.Both);
            Assert.True(bridge.Read(token).Closed);
            Assert.Throws<InvalidOperationException>(() => bridge.Read(token));
        }
        finally { listener.Stop(); }
    }

    [Fact]
    public async Task ExpiresAnIdleSessionOnTheInjectedClock()
    {
        var address = Dns.GetHostAddresses(Dns.GetHostName()).First(ip =>
            ip.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(ip));
        var listener = new TcpListener(address, 2323);
        listener.Start();
        try
        {
            var time = new ManualTime();
            using var bridge = new ConsoleBridge(time);
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var opening = Task.Run(() => bridge.Open(Wireless(address.ToString()), (_, _) => new byte[16]));
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            await peer.SendAsync(Encoding.ASCII.GetBytes(
                "PicoForge AUTH 00000000000000000000000000000000 0001020304050607\r\n"),
                SocketFlags.None, timeout.Token);
            var token = await opening.WaitAsync(timeout.Token);
            time.Advance(TimeSpan.FromSeconds(14));
            bridge.Expire();
            Assert.Empty(bridge.Read(token).Data);
            time.Advance(TimeSpan.FromSeconds(15));
            bridge.Expire();
            Assert.Throws<InvalidOperationException>(() => bridge.Read(token));
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
    public void DoesNotUseInvalidBoardAddressesAsProxyTargets(string address)
    {
        using var bridge = new ConsoleBridge();
        Assert.Throws<ArgumentException>(() => bridge.Open(Wireless(address), null));
    }

    [Fact]
    public void DoesNotConnectWhenTheListenerHasNotStarted()
    {
        using var bridge = new ConsoleBridge();
        Assert.Throws<InvalidOperationException>(() =>
            bridge.Open(Wireless("192.168.66.87") with { TcpCliStarted = false }, null));
    }

    [Fact]
    public async Task AuthenticatesTheTcpCliWithTheSavedBleKey()
    {
        var address = Dns.GetHostAddresses(Dns.GetHostName()).First(ip =>
            ip.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(ip));
        var listener = new TcpListener(address, 2323);
        listener.Start();
        try
        {
            using var bridge = new ConsoleBridge();
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var key = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
            var nonce = Enumerable.Range(0, 16).Select(value => (byte)value).ToArray();
            var uid = Enumerable.Range(0, 8).Select(value => (byte)value).ToArray();
            var opening = Task.Run(() => bridge.Open(Wireless(address.ToString(), true), (challenge, boardUid) =>
                HMACSHA256.HashData(key, challenge.Concat(boardUid)
                    .Concat(Encoding.ASCII.GetBytes("PFTCPCLI1TCPCLI001")).ToArray()).AsSpan(0, 16).ToArray()));
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
    public void RequiresUsbProofSupportWhenTheBoardRequiresTcpAuthentication()
    {
        using var bridge = new ConsoleBridge();
        Assert.Throws<InvalidOperationException>(() => bridge.Open(Wireless("192.168.66.87", true)));
    }
}
