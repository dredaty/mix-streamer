#pragma once
#include <juce_core/juce_core.h>
#include <functional>

/**
 * Minimal RFC 6455 WebSocket client on top of juce::StreamingSocket.
 * Plain ws:// only (used for the local relay on 127.0.0.1).
 * Not thread-safe: use from a single (background) thread.
 */
class WebSocketClient
{
public:
    WebSocketClient();
    ~WebSocketClient();

    bool connect (const juce::String& host, int port, const juce::String& path, int timeoutMs);
    void disconnect();
    bool isConnected() const noexcept { return connected; }

    /** Sends one binary message (single frame). Returns false on error (connection is closed). */
    bool sendBinary (const void* data, size_t numBytes);

    /** Non-blocking: reads whatever arrived, answers pings, delivers text messages.
        Returns false if the connection dropped. */
    bool poll();

    std::function<void (const juce::String&)> onTextMessage;
    std::function<void (const juce::uint8*, size_t)> onBinaryMessage;

private:
    bool sendFrame (juce::uint8 opcode, const void* data, size_t numBytes);
    bool readHandshake (int timeoutMs);
    void parseFrames();

    juce::StreamingSocket socket;
    bool connected = false;
    juce::MemoryBlock rx;          // received, not yet parsed bytes
    size_t rxUsed = 0;
    juce::MemoryBlock tx;          // reused frame buffer
    juce::Random rng;
};
