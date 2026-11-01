using System.Net;
using System.Net.Sockets;
using System.Text;
using PicoForge;
using Xunit;

namespace PicoForge.Tests;

public sealed class ConsoleBridgeTests
{
    private static WirelessStatusResponse Wireless(string address) =>
        new(true, false, "HomeDev", true, false, "idle", 0, "connected", address, 0, TcpCliStarted: true);

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
            var token = bridge.Open(Wireless(address.ToString()));
            using var peer = await listener.AcceptSocketAsync(timeout.Token);
            Assert.Throws<InvalidOperationException>(() => bridge.Open(Wireless(address.ToString())));
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
        Assert.Throws<ArgumentException>(() => bridge.Open(Wireless(address)));
    }

    [Fact]
    public void DoesNotConnectWhenTheListenerHasNotStarted()
    {
        using var bridge = new ConsoleBridge();
        Assert.Throws<InvalidOperationException>(() => bridge.Open(Wireless("192.168.66.87") with
            { TcpCliStarted = false }));
    }
}
