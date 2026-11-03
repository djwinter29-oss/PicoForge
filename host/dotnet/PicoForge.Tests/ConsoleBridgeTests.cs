using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ConsoleBridgeTests
{
    private static WirelessStatusResponse Wireless(string address, bool authRequired = false) =>
        new(true, false, "HomeDev", true, true, "idle", 0, "connected", address, 0,
            TcpCliStarted: true, TcpCliAuthRequired: authRequired);

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
            var token = bridge.Open(Wireless(address.ToString()), null);
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
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
            var opening = Task.Run(() => bridge.Open(Wireless(address.ToString(), true), Convert.ToHexString(key)));
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
    public void RequiresANonzeroBleKeyWhenTheBoardRequiresTcpAuthentication()
    {
        using var bridge = new ConsoleBridge();
        Assert.Throws<ArgumentException>(() => bridge.Open(Wireless("192.168.66.87", true), null));
        Assert.Throws<ArgumentException>(() =>
            bridge.Open(Wireless("192.168.66.87", true), new string('0', 64)));
    }
}
