using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;

namespace PicoForge;

/// <summary>Bounded single-session raw TCP bridge; abandoned browser tabs expire after 15 seconds.</summary>
internal sealed class ConsoleBridge : IDisposable
{
    private readonly object gate = new();
    private readonly TimeProvider time;
    private readonly ITimer expiry;
    private Socket? connection;
    private string? session;
    // TimeProvider timestamp of the last session request, guarded by gate.
    private long lastRequest;

    public ConsoleBridge(TimeProvider? time = null)
    {
        this.time = time ?? TimeProvider.System;
        expiry = this.time.CreateTimer(_ => Expire(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    public string Open(WirelessStatusResponse wireless, Func<byte[], byte[], byte[]>? signProof = null)
    {
        if (!wireless.WifiEnabled || wireless.WifiState != "connected" || !wireless.TcpCliStarted)
            throw new InvalidOperationException("The board's Wi-Fi TCP CLI is not available");
        if (!IPAddress.TryParse(wireless.Ipv4, out var address) ||
            address.AddressFamily != AddressFamily.InterNetwork || IPAddress.IsLoopback(address) ||
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
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            socket.ConnectAsync(new IPEndPoint(address, 2323), timeout.Token).AsTask().GetAwaiter().GetResult();
            Authenticate(socket, signProof);
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

    private void Authenticate(Socket socket, Func<byte[], byte[], byte[]> signProof)
    {
        var challenge = ReadChallenge(socket);
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
            proof = signProof(nonce, uid);
            if (proof.Length != 16) throw new InvalidOperationException("The board returned an invalid TCP CLI proof");
            var response = System.Text.Encoding.ASCII.GetBytes(
                $"AUTH {Convert.ToHexString(proof).ToLowerInvariant()}\r\n");
            try { SendAll(socket, response); }
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

    private string ReadChallenge(Socket socket)
    {
        var line = new List<byte>(96);
        var value = new byte[1];
        var started = time.GetTimestamp();
        while (line.Count < 128 && time.GetElapsedTime(started) < TimeSpan.FromSeconds(3))
        {
            if (!socket.Poll(100_000, SelectMode.SelectRead)) continue;
            var count = socket.Receive(value, SocketFlags.None);
            if (count == 0) throw new IOException("Board closed the TCP CLI during authentication");
            line.Add(value[0]);
            if (line.Count >= 2 && line[^2] == '\r' && line[^1] == '\n')
                return System.Text.Encoding.ASCII.GetString([.. line]);
        }
        throw new IOException("TCP CLI authentication challenge timed out or exceeded its limit");
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
    public (byte[] Data, bool Closed) Read(string? token)
    {
        Socket waiting;
        lock (gate) waiting = RequireSession(token);
        // ponytail: one long-poll GET per 2 s idle replaces 20 polls/s; a WebSocket would only cut the per-chunk
        // request.
        bool ready;
        try { ready = waiting.Poll(2_000_000, SelectMode.SelectRead); }
        catch (Exception error) when (error is ObjectDisposedException or SocketException) { ready = false; }
        lock (gate)
        {
            var socket = RequireSession(token);
            if (!ReferenceEquals(socket, waiting))
                throw new InvalidOperationException("Console disconnected; connect again");
            try
            {
                if (!ready) return ([], false);
                var data = new byte[4096];
                var count = socket.Receive(data);
                if (count == 0) CloseConnection();
                return (data[..count], count == 0);
            }
            catch (SocketException)
            {
                CloseConnection();
                throw;
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

    /// <summary>
    /// Maps the console routes; <see cref="StatusEndpoints.UseApiErrors"/> supplies every error body.
    /// </summary>
    public static void MapEndpoints(WebApplication app)
    {
        var unavailable = new StatusEndpoints.UnavailableMessage("TCP console connection failed", WithDetail: true);
        var limit = new Microsoft.AspNetCore.Mvc.RequestSizeLimitAttribute(2048);
        app.MapPost("/api/console", (JsonElement payload, string? board, IDashboardBoards dashboard,
            ConsoleBridge bridge, CancellationToken cancel) =>
        {
            if (payload.ValueKind != JsonValueKind.Object || payload.EnumerateObject().Any())
                throw new ArgumentException("Expected an empty object; TCP authentication uses the connected board");
            var monitor = dashboard.Monitor(board);
            var wireless = monitor.ReadWireless(cancel).Wireless;
            return Results.Ok(new
            {
                session = bridge.Open(wireless, (nonce, uid) => monitor.TcpCliAuthProof(nonce, uid, cancel))
            });
        }).Errors(StatusEndpoints.CommandErrors).WithMetadata(unavailable, limit);
        app.MapGet("/api/console", (string? session, ConsoleBridge bridge) =>
        {
            var (data, closed) = bridge.Read(session);
            return Results.Ok(new { data = Convert.ToBase64String(data), closed });
        }).Errors(409, 503).WithMetadata(unavailable);
        app.MapPut("/api/console", (JsonElement payload, string? session, ConsoleBridge bridge) =>
        {
            if (payload.ValueKind != JsonValueKind.Object || payload.EnumerateObject().Count() != 1 ||
                !payload.TryGetProperty("data", out var field) || field.ValueKind != JsonValueKind.String)
                throw new ArgumentException("Expected base64-encoded input bytes");
            var encoded = field.GetString()!;
            if (encoded.Length > 1368) throw new ArgumentException("Input exceeds 1024 bytes");
            var data = Convert.FromBase64String(encoded);
            if (data.Length is < 1 or > 1024) throw new ArgumentException("Expected 1..1024 input bytes");
            bridge.Write(session, data);
            return Results.Ok(new { ok = true });
        }).Errors(400, 409, 503).WithMetadata(unavailable, limit);
        app.MapDelete("/api/console", (string? session, ConsoleBridge bridge) =>
        {
            bridge.Close(session);
            return Results.Ok(new { ok = true });
        }).Errors(409);
    }
}
