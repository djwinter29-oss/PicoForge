using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;

namespace PicoForge;

/// <summary>Bounded single-session raw TCP bridge; abandoned browser tabs expire after 15 seconds.</summary>
internal sealed class ConsoleBridge : IDisposable
{
    private readonly object gate = new();
    private readonly TimeProvider time;
    private readonly ITimer expiry;
    private readonly int port;
    private readonly bool allowLoopbackForTests;
    private Socket? connection;
    private string? session;
    // TimeProvider timestamp of the last session request, guarded by gate.
    private long lastRequest;

    public ConsoleBridge(TimeProvider? time = null)
        : this(time, 2323, false) { }

    internal ConsoleBridge(TimeProvider? time, int port, bool allowLoopbackForTests)
    {
        this.time = time ?? TimeProvider.System;
        this.port = port;
        this.allowLoopbackForTests = allowLoopbackForTests;
        expiry = this.time.CreateTimer(_ => Expire(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    public async Task<string> OpenAsync(WirelessStatusResponse wireless,
        Func<byte[], byte[], byte[]>? signProof = null, CancellationToken cancel = default)
    {
        if (!wireless.WifiEnabled || wireless.WifiState != WifiState.Connected || !wireless.TcpCliStarted)
            throw new InvalidOperationException("The board's Wi-Fi TCP CLI is not available");
        if (!IPAddress.TryParse(wireless.Ipv4, out var address) ||
            address.AddressFamily != AddressFamily.InterNetwork ||
            !allowLoopbackForTests && IPAddress.IsLoopback(address) ||
            address.Equals(IPAddress.Any) || address.Equals(IPAddress.Broadcast) ||
            address.GetAddressBytes()[0] is >= 224 and <= 239)
            throw new ArgumentException("Invalid board IPv4 address");
        if (!wireless.BleKeyConfigured || signProof is null)
            throw new InvalidOperationException("USB Control key proof is unavailable");
        const string Busy = "A web console is already connected; disconnect it first";
        lock (gate)
            if (connection is not null) throw new InvalidOperationException(Busy);
        // Connect and authenticate (up to 6 s) without the gate, so reads, writes and expiry never wait on them.
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            timeout.CancelAfter(TimeSpan.FromSeconds(3));
            await socket.ConnectAsync(new IPEndPoint(address, port), timeout.Token);
            await AuthenticateAsync(socket, signProof, cancel);
            socket.SendTimeout = 100;
            lock (gate)
            {
                if (connection is not null) throw new InvalidOperationException(Busy);
                connection = socket;
                session = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
                lastRequest = time.GetTimestamp();
                return session;
            }
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    private async Task AuthenticateAsync(Socket socket, Func<byte[], byte[], byte[]> signProof,
        CancellationToken cancel)
    {
        var challenge = await ReadChallengeAsync(socket, cancel);
        var fields = challenge[..^2].Split(' ');
        if (fields.Length != 4 || fields[0] != "PicoForge" || fields[1] != "AUTH" ||
            fields[2].Length != 32 || fields[2].Any(character => !Uri.IsHexDigit(character)) ||
            fields[3].Length != 16 || fields[3].Any(character => !Uri.IsHexDigit(character)))
            throw new IOException("Invalid TCP CLI authentication challenge");
        byte[] nonce = Convert.FromHexString(fields[2]);
        byte[] uid = Convert.FromHexString(fields[3]);
        byte[] proof = [];
        try
        {
            cancel.ThrowIfCancellationRequested();
            proof = signProof(nonce, uid);
            if (proof.Length != 16) throw new InvalidOperationException("The board returned an invalid TCP CLI proof");
            var response = System.Text.Encoding.ASCII.GetBytes(
                $"AUTH {Convert.ToHexString(proof).ToLowerInvariant()}\r\n");
            try { await SendAllAsync(socket, response, cancel); }
            finally
            {
                CryptographicOperations.ZeroMemory(response);
            }
        }
        finally
        {
            CryptographicOperations.ZeroMemory(nonce);
            CryptographicOperations.ZeroMemory(uid);
            CryptographicOperations.ZeroMemory(proof);
        }
    }

    private async Task<string> ReadChallengeAsync(Socket socket, CancellationToken cancel)
    {
        var line = new List<byte>(96);
        var value = new byte[1];
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        timeout.CancelAfter(TimeSpan.FromSeconds(3));
        try
        {
            while (line.Count < 128)
            {
                var count = await socket.ReceiveAsync(value, SocketFlags.None, timeout.Token);
                if (count == 0) throw new IOException("Board closed the TCP CLI during authentication");
                line.Add(value[0]);
                if (line.Count >= 2 && line[^2] == '\r' && line[^1] == '\n')
                    return System.Text.Encoding.ASCII.GetString([.. line]);
            }
        }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested)
        {
            throw new IOException("TCP CLI authentication challenge timed out or exceeded its limit");
        }
        throw new IOException("TCP CLI authentication challenge timed out or exceeded its limit");
    }

    private static async Task SendAllAsync(Socket socket, ReadOnlyMemory<byte> data, CancellationToken cancel)
    {
        var sent = 0;
        while (sent < data.Length)
        {
            var count = await socket.SendAsync(data[sent..], SocketFlags.None, cancel);
            if (count == 0) throw new SocketException((int)SocketError.ConnectionReset);
            sent += count;
        }
    }

    private static void SendAll(Socket socket, ReadOnlySpan<byte> data)
    {
        while (!data.IsEmpty)
        {
            var count = socket.Send(data, SocketFlags.None);
            if (count == 0) throw new SocketException((int)SocketError.ConnectionReset);
            data = data[count..];
        }
    }

    private Socket RequireSession(string? token)
    {
        if (connection is null || token != session)
            throw new InvalidOperationException("Console disconnected; connect again");
        lastRequest = time.GetTimestamp();
        return connection;
    }

    /// <summary>Waits up to 2 s for board output without holding the gate, so writes are never blocked.</summary>
    public async Task<(byte[] Data, bool Closed)> ReadAsync(string? token, CancellationToken cancel = default)
    {
        Socket waiting;
        lock (gate) waiting = RequireSession(token);
        var data = new byte[4096];
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        timeout.CancelAfter(TimeSpan.FromSeconds(2));
        int count;
        try { count = await waiting.ReceiveAsync(data, SocketFlags.None, timeout.Token); }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested)
        {
            count = -1;
        }
        catch (Exception error) when (error is ObjectDisposedException or SocketException)
        {
            lock (gate)
            {
                if (!ReferenceEquals(connection, waiting))
                    throw new InvalidOperationException("Console disconnected; connect again");
                CloseConnection();
            }
            throw new IOException("TCP console connection failed", error);
        }
        lock (gate)
        {
            var socket = RequireSession(token);
            if (!ReferenceEquals(socket, waiting))
                throw new InvalidOperationException("Console disconnected; connect again");
            try
            {
                if (count < 0) return ([], false);
                if (count == 0) CloseConnection();
                return (data[..count], count == 0);
            }
            catch (Exception error) when (error is ObjectDisposedException or SocketException)
            {
                CloseConnection();
                throw new IOException("TCP console connection failed", error);
            }
        }
    }

    public void Write(string? token, byte[] data)
    {
        lock (gate)
        {
            var socket = RequireSession(token);
            try { SendAll(socket, data); }
            catch (SocketException)
            {
                CloseConnection();
                throw;
            }
        }
    }

    public void Close(string? token)
    {
        lock (gate)
        {
            RequireSession(token);
            CloseConnection();
        }
    }

    private void CloseConnection()
    {
        connection?.Dispose();
        connection = null;
        session = null;
    }

    /// <summary>Closes a session idle for 15 s; the timer calls it every second.</summary>
    internal void Expire()
    {
        lock (gate)
            if (connection is not null && time.GetElapsedTime(lastRequest) >= TimeSpan.FromSeconds(15))
                CloseConnection();
    }

    public void Dispose()
    {
        expiry.Dispose();
        lock (gate) CloseConnection();
    }
}
