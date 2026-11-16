#include "WebSocketClient.h"

WebSocketClient::WebSocketClient()
{
    rx.setSize (64 * 1024);
    tx.setSize (256 * 1024);
}

WebSocketClient::~WebSocketClient() { disconnect(); }

bool WebSocketClient::connect (const juce::String& host, int port, const juce::String& path, int timeoutMs)
{
    disconnect();

    if (! socket.connect (host, port, timeoutMs))
        return false;

    // 16 random bytes, base64 → Sec-WebSocket-Key
    juce::uint8 key[16];
    for (auto& b : key)
        b = (juce::uint8) rng.nextInt (256);

    const juce::String request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + ":" + juce::String (port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + juce::Base64::toBase64 (key, sizeof (key)) + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: MixStreamer-VST3\r\n\r\n";

    const auto utf8 = request.toRawUTF8();
    const int len = (int) strlen (utf8);
    if (socket.write (utf8, len) != len || ! readHandshake (timeoutMs))
    {
        socket.close();
        return false;
    }

    connected = true;
    return true;
}

bool WebSocketClient::readHandshake (int timeoutMs)
{
    // Read byte by byte up to "\r\n\r\n" so that no frame data gets swallowed.
    juce::String header;
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    rxUsed = 0;

    while (header.length() < 4096)
    {
        const int remaining = (int) (deadline - juce::Time::getMillisecondCounter());
        if (remaining <= 0 || socket.waitUntilReady (true, remaining) != 1)
            return false;

        char c;
        if (socket.read (&c, 1, true) != 1)
            return false;

        header += c;
        if (header.endsWith ("\r\n\r\n"))
            return header.startsWith ("HTTP/1.1 101");
    }
    return false;
}

void WebSocketClient::disconnect()
{
    if (connected)
    {
        const juce::uint8 code[2] = { 0x03, 0xE8 }; // 1000 normal closure
        sendFrame (0x8, code, 2);
    }
    connected = false;
    socket.close();
    rxUsed = 0;
}

bool WebSocketClient::sendBinary (const void* data, size_t numBytes)
{
    return sendFrame (0x2, data, numBytes);
}

bool WebSocketClient::sendFrame (juce::uint8 opcode, const void* data, size_t numBytes)
{
    if (! socket.isConnected())
        return false;

    const size_t maxHeader = 14;
    if (tx.getSize() < numBytes + maxHeader)
        tx.setSize (numBytes + maxHeader);

    auto* out = static_cast<juce::uint8*> (tx.getData());
    size_t pos = 0;
    out[pos++] = (juce::uint8) (0x80 | opcode); // FIN + opcode

    if (numBytes < 126)
    {
        out[pos++] = (juce::uint8) (0x80 | numBytes);
    }
    else if (numBytes <= 0xFFFF)
    {
        out[pos++] = 0x80 | 126;
        out[pos++] = (juce::uint8) (numBytes >> 8);
        out[pos++] = (juce::uint8) (numBytes & 0xFF);
    }
    else
    {
        out[pos++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i)
            out[pos++] = (juce::uint8) (((juce::uint64) numBytes >> (8 * i)) & 0xFF);
    }

    // Client → server frames must be masked.
    juce::uint8 mask[4];
    const auto r = (juce::uint32) rng.nextInt();
    mask[0] = (juce::uint8) r; mask[1] = (juce::uint8) (r >> 8);
    mask[2] = (juce::uint8) (r >> 16); mask[3] = (juce::uint8) (r >> 24);
    memcpy (out + pos, mask, 4);
    pos += 4;

    const auto* in = static_cast<const juce::uint8*> (data);
    for (size_t i = 0; i < numBytes; ++i)
        out[pos + i] = in[i] ^ mask[i & 3];
    pos += numBytes;

    // Single write per frame (avoids Nagle/delayed-ACK stalls).
    if (socket.write (out, (int) pos) != (int) pos)
    {
        connected = false;
        socket.close();
        return false;
    }
    return true;
}

bool WebSocketClient::poll()
{
    if (! connected)
        return false;

    while (socket.waitUntilReady (true, 0) == 1)
    {
        if (rxUsed + 4096 > rx.getSize())
            rx.setSize (rx.getSize() * 2);

        const int n = socket.read (static_cast<char*> (rx.getData()) + rxUsed, 4096, false);
        if (n <= 0) // 0 after "ready" = peer closed
        {
            connected = false;
            socket.close();
            return false;
        }
        rxUsed += (size_t) n;
    }

    parseFrames();
    return connected;
}

void WebSocketClient::parseFrames()
{
    auto* buf = static_cast<juce::uint8*> (rx.getData());
    size_t offset = 0;

    while (rxUsed - offset >= 2)
    {
        const auto* f = buf + offset;
        const juce::uint8 opcode = f[0] & 0x0F;
        const bool masked = (f[1] & 0x80) != 0;
        juce::uint64 len = f[1] & 0x7F;
        size_t hdr = 2;

        if (len == 126)
        {
            if (rxUsed - offset < 4) break;
            len = ((juce::uint64) f[2] << 8) | f[3];
            hdr = 4;
        }
        else if (len == 127)
        {
            if (rxUsed - offset < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | f[2 + i];
            hdr = 10;
        }

        if (masked) hdr += 4;
        if (len > (16u << 20)) { disconnect(); return; } // sanity limit
        if (rxUsed - offset < hdr + len) break;           // incomplete

        auto* payload = const_cast<juce::uint8*> (f + hdr);
        if (masked)
            for (size_t i = 0; i < len; ++i)
                payload[i] ^= f[hdr - 4 + (i & 3)];

        switch (opcode)
        {
            case 0x1: // text
                if (onTextMessage)
                    onTextMessage (juce::String::fromUTF8 (reinterpret_cast<const char*> (payload), (int) len));
                break;
            case 0x8: // close
                disconnect();
                return;
            case 0x9: // ping → pong
                sendFrame (0xA, payload, (size_t) len);
                break;
            default:  // binary / pong / continuation: ignored
                break;
        }
        offset += hdr + (size_t) len;
    }

    if (offset > 0)
    {
        memmove (buf, buf + offset, rxUsed - offset);
        rxUsed -= offset;
    }
}
