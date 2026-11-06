using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

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
    private Socket? opening;
    private bool closed;
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
        expiry = this.time.CreateTimer(_ => Expire(), null, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
    }

    public async Task<string> OpenAsync(WirelessStatusResponse wireless,
        Func<byte[], byte[], byte[]>? signProof = null, CancellationToken cancel = default)
    {
        if (!wireless.WifiEnabled || wireless.WifiState != WifiState.Connected || !wireless.TcpCliStarted)
            throw new BoardStateException("The board's Wi-Fi TCP CLI is not available");
        if (!IPAddress.TryParse(wireless.Ipv4, out var address) ||
            address.AddressFamily != AddressFamily.InterNetwork ||
            !allowLoopbackForTests && IPAddress.IsLoopback(address) ||
            address.Equals(IPAddress.Any) || address.Equals(IPAddress.Broadcast) ||
            address.GetAddressBytes()[0] is >= 224 and <= 239)
            throw new InvalidDataException("Invalid board IPv4 address");
        if (!wireless.BleKeyConfigured || signProof is null)
            throw new BoardStateException("USB Control key proof is unavailable");
        const string Busy = "A web console is already connected; disconnect it first";
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        try
        {
            lock (gate)
            {
                if (closed) throw new BoardStateException("Console bridge is shutting down");
                if (connection is not null || opening is not null) throw new BoardStateException(Busy);
                opening = socket;
            }
            // Neither the 3 s connect nor the 8 s authentication budget holds the session lock.
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            timeout.CancelAfter(TimeSpan.FromSeconds(3));
            await socket.ConnectAsync(new IPEndPoint(address, port), timeout.Token);
            await AuthenticateAsync(socket, signProof, cancel);
            socket.SendTimeout = 100;
            lock (gate)
            {
                if (closed) throw new BoardStateException("Console bridge is shutting down");
                if (connection is not null) throw new BoardStateException(Busy);
                connection = socket;
                session = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
                lastRequest = time.GetTimestamp();
                expiry.Change(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
                return session;
            }
        }
        catch (Exception error)
        {
            socket.Dispose();
            lock (gate)
                if (closed && error is ObjectDisposedException or SocketException or IOException
                    or OperationCanceledException or BoardStateException)
                    throw new BoardStateException("Console bridge is shutting down");
            throw;
        }
        finally
        {
            lock (gate)
                if (ReferenceEquals(opening, socket)) opening = null;
        }
    }

    private async Task AuthenticateAsync(Socket socket, Func<byte[], byte[], byte[]> signProof,
        CancellationToken cancel)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        timeout.CancelAfter(TimeSpan.FromSeconds(8));
        var started = time.GetTimestamp();
        var challenge = await ReadChallengeAsync(socket, timeout.Token);
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
            timeout.Token.ThrowIfCancellationRequested();
            proof = signProof(nonce, uid);
            timeout.Token.ThrowIfCancellationRequested();
            if (time.GetElapsedTime(started) >= TimeSpan.FromSeconds(8))
                throw new TimeoutException("TCP CLI authentication timed out");
            if (proof.Length != 16) throw new InvalidDataException("The board returned an invalid TCP CLI proof");
            var response = System.Text.Encoding.ASCII.GetBytes(
                $"AUTH {Convert.ToHexString(proof).ToLowerInvariant()}\r\n");
            try { await SendAllAsync(socket, response, timeout.Token); }
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
        if (connection is null || session is null || !MatchesSession(token, session))
            throw new BoardStateException("Console disconnected; connect again");
        lastRequest = time.GetTimestamp();
        return connection;
    }

    private static bool MatchesSession(string? token, string session)
    {
        if (token is null || token.Length != session.Length) return false;
        var supplied = Encoding.ASCII.GetBytes(token);
        var expected = Encoding.ASCII.GetBytes(session);
        try { return CryptographicOperations.FixedTimeEquals(supplied, expected); }
        finally
        {
            CryptographicOperations.ZeroMemory(supplied);
            CryptographicOperations.ZeroMemory(expected);
        }
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
                    throw new BoardStateException("Console disconnected; connect again");
                CloseConnection();
            }
            throw new IOException("TCP console connection failed", error);
        }
        lock (gate)
        {
            var socket = RequireSession(token);
            if (!ReferenceEquals(socket, waiting))
                throw new BoardStateException("Console disconnected; connect again");
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
        expiry.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
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
        lock (gate)
        {
            if (closed) return;
            closed = true;
            opening?.Dispose();
            opening = null;
            CloseConnection();
            expiry.Dispose();
        }
    }
}
