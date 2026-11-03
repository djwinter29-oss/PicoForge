using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;

namespace PicoForge;

/// <summary>Bounded single-session raw TCP bridge; abandoned browser tabs expire after 15 seconds.</summary>
internal sealed class ConsoleBridge : IDisposable
{
    private readonly object gate = new();
    private readonly Timer expiry;
    private Socket? connection;
    private string? session;
    private long lastRequest;

    public ConsoleBridge() => expiry = new Timer(_ => Expire(), null, 1000, 1000);

    public string Open(WirelessStatusResponse wireless, string? bleKey)
    {
        if (!wireless.WifiEnabled || wireless.WifiState != "connected" || !wireless.TcpCliStarted)
            throw new InvalidOperationException("The board's Wi-Fi TCP CLI is not available");
        if (!IPAddress.TryParse(wireless.Ipv4, out var address) ||
            address.AddressFamily != AddressFamily.InterNetwork || IPAddress.IsLoopback(address) ||
            address.Equals(IPAddress.Any) || address.Equals(IPAddress.Broadcast) ||
            address.GetAddressBytes()[0] is >= 224 and <= 239)
            throw new ArgumentException("Invalid board IPv4 address");
        if (wireless.TcpCliAuthRequired &&
            (bleKey is null || bleKey.Length != 64 || bleKey.Any(character => !Uri.IsHexDigit(character)) ||
             bleKey.All(character => character == '0')))
            throw new ArgumentException("A 64-character nonzero BLE control key is required");
        lock (gate)
        {
            if (connection is not null)
                throw new InvalidOperationException("A web console is already connected; disconnect it first");
            var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            try
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                socket.ConnectAsync(new IPEndPoint(address, 2323), timeout.Token).AsTask().GetAwaiter().GetResult();
                if (wireless.TcpCliAuthRequired) Authenticate(socket, bleKey!);
                socket.SendTimeout = 100;
                connection = socket;
                session = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
                lastRequest = Environment.TickCount64;
                return session;
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }
    }

    private static void Authenticate(Socket socket, string bleKey)
    {
        var challenge = ReadChallenge(socket);
        var fields = challenge[..^2].Split(' ');
        if (fields.Length != 4 || fields[0] != "PicoForge" || fields[1] != "AUTH" ||
            fields[2].Length != 32 || fields[2].Any(character => !Uri.IsHexDigit(character)) ||
            fields[3].Length != 16 || fields[3].Any(character => !Uri.IsHexDigit(character)))
            throw new IOException("Invalid TCP CLI authentication challenge");
        byte[] key = Convert.FromHexString(bleKey);
        byte[] nonce = Convert.FromHexString(fields[2]);
        byte[] uid = Convert.FromHexString(fields[3]);
        byte[] message = new byte[42];
        byte[] digest = [];
        try
        {
            nonce.CopyTo(message, 0);
            uid.CopyTo(message, 16);
            "PFTCPCLI1"u8.CopyTo(message.AsSpan(24));
            "TCPCLI001"u8.CopyTo(message.AsSpan(33));
            digest = HMACSHA256.HashData(key, message);
            var response = System.Text.Encoding.ASCII.GetBytes(
                $"AUTH {Convert.ToHexString(digest.AsSpan(0, 16)).ToLowerInvariant()}\r\n");
            try
            {
                var sent = 0;
                while (sent < response.Length)
                {
                    var count = socket.Send(response.AsSpan(sent), SocketFlags.None);
                    if (count == 0) throw new SocketException((int)SocketError.ConnectionReset);
                    sent += count;
                }
            }
            finally
            {
                CryptographicOperations.ZeroMemory(response);
            }
        }
        finally
        {
            CryptographicOperations.ZeroMemory(key);
            CryptographicOperations.ZeroMemory(nonce);
            CryptographicOperations.ZeroMemory(uid);
            CryptographicOperations.ZeroMemory(message);
            CryptographicOperations.ZeroMemory(digest);
        }
    }

    private static string ReadChallenge(Socket socket)
    {
        var line = new List<byte>(96);
        var deadline = Environment.TickCount64 + 3000;
        while (line.Count < 128 && Environment.TickCount64 < deadline)
        {
            if (!socket.Poll(100_000, SelectMode.SelectRead)) continue;
            var value = new byte[1];
            var count = socket.Receive(value, SocketFlags.None);
            if (count == 0) throw new IOException("Board closed the TCP CLI during authentication");
            line.Add(value[0]);
            if (line.Count >= 2 && line[^2] == '\r' && line[^1] == '\n')
                return System.Text.Encoding.ASCII.GetString([.. line]);
        }
        throw new IOException("TCP CLI authentication challenge timed out or exceeded its limit");
    }

    private Socket RequireSession(string? token)
    {
        if (connection is null || token != session)
            throw new InvalidOperationException("Console disconnected; connect again");
        lastRequest = Environment.TickCount64;
        return connection;
    }

    public (byte[] Data, bool Closed) Read(string? token)
    {
        lock (gate)
        {
            var socket = RequireSession(token);
            try
            {
                if (!socket.Poll(100_000, SelectMode.SelectRead)) return ([], false);
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
            try
            {
                var sent = 0;
                while (sent < data.Length)
                {
                    var count = socket.Send(data.AsSpan(sent), SocketFlags.None);
                    if (count == 0) throw new SocketException((int)SocketError.ConnectionReset);
                    sent += count;
                }
            }
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

    private void Expire()
    {
        lock (gate)
            if (connection is not null && Environment.TickCount64 - lastRequest >= 15_000) CloseConnection();
    }

    public void Dispose()
    {
        expiry.Dispose();
        lock (gate) CloseConnection();
    }

    public static void MapEndpoints(WebApplication app)
    {
        app.MapMethods("/api/console", ["POST", "GET", "PUT", "DELETE"],
            async (HttpContext context, IDashboardBoards dashboard, ConsoleBridge bridge, ILogger<ConsoleBridge> log) =>
        {
            var request = context.Request;
            if (request.Headers.TryGetValue("Origin", out var origin) &&
                origin != $"{request.Scheme}://{request.Host}")
                return Results.Json(new { error = "Console requests must be same-origin" }, statusCode: 403);
            try
            {
                var token = request.Query["session"].ToString();
                if (request.Method == "POST")
                {
                    var payload = await request.ReadFromJsonAsync<JsonElement>(context.RequestAborted);
                    if (payload.ValueKind != JsonValueKind.Object ||
                        payload.EnumerateObject().Any(property => property.Name != "ble_key") ||
                        payload.TryGetProperty("ble_key", out var keyField) &&
                        keyField.ValueKind != JsonValueKind.String)
                        throw new ArgumentException("Expected an empty object or a BLE control key");
                    var bleKey = payload.TryGetProperty("ble_key", out keyField) ? keyField.GetString() : null;
                    var wireless = dashboard.ReadWireless(request.Query["board"].ToString()).Wireless;
                    return Results.Ok(new { session = bridge.Open(wireless, bleKey) });
                }
                if (request.Method == "GET")
                {
                    var (data, closed) = bridge.Read(token);
                    return Results.Ok(new { data = Convert.ToBase64String(data), closed });
                }
                if (request.Method == "PUT")
                {
                    var payload = await request.ReadFromJsonAsync<JsonElement>(context.RequestAborted);
                    if (payload.ValueKind != JsonValueKind.Object || payload.EnumerateObject().Count() != 1 ||
                        !payload.TryGetProperty("data", out var field) || field.ValueKind != JsonValueKind.String)
                        throw new ArgumentException("Expected base64-encoded input bytes");
                    var encoded = field.GetString()!;
                    if (encoded.Length > 1368) throw new ArgumentException("Input exceeds 1024 bytes");
                    var data = Convert.FromBase64String(encoded);
                    if (data.Length is < 1 or > 1024) throw new ArgumentException("Expected 1..1024 input bytes");
                    bridge.Write(token, data);
                }
                else bridge.Close(token);
                return Results.Ok(new { ok = true });
            }
            catch (Exception error) when (error is ArgumentException or FormatException or JsonException)
            {
                return Results.Json(new { error = error.Message }, statusCode: 400);
            }
            catch (InvalidOperationException error)
            {
                return Results.Json(new { error = error.Message }, statusCode: 409);
            }
            catch (Exception error) when (error is SocketException or IOException or OperationCanceledException)
            {
                log.LogWarning(error, "TCP console connection failed");
                return Results.Json(new { error = $"TCP console connection failed: {error.Message}" }, statusCode: 503);
            }
        }).WithMetadata(new Microsoft.AspNetCore.Mvc.RequestSizeLimitAttribute(2048));
    }
}
