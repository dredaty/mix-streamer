#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>

using Packet = std::shared_ptr<const juce::MemoryBlock>;

class ListenerConnection;
class CloudflaredTunnel;

enum class TunnelState { idle, starting, online, missingBinary, disabled, error };

/**
 * Process-wide singleton (use via juce::SharedResourcePointer<StreamHub>).
 *
 * Embedded relay: a small HTTP server on 127.0.0.1:<random port> that serves the web
 * player and the /listen WebSocket, plus a managed `cloudflared` quick tunnel that
 * makes it reachable at https://…trycloudflare.com.
 *
 * Nothing is started until activate() is called (the plug-in does that only once real
 * audio flows), so DAW plug-in scans never spawn a tunnel.
 */
class StreamHub : private juce::Thread
{
public:
    StreamHub();
    ~StreamHub() override;

    /** Starts the HTTP server and tunnel. Thread-safe and idempotent. */
    void activate();
    bool isActive() const noexcept                 { return active.load(); }

    /** Only one plug-in instance streams at a time. */
    bool claimSource (const void* owner) noexcept;
    void releaseSource (const void* owner) noexcept;
    bool isSource (const void* owner) const noexcept { return source.load() == owner; }

    /** Called from the sender thread with a complete header+PCM packet. */
    void publish (const void* owner, Packet packet, juce::uint32 sampleRate, int bitDepth);

    int getListenerCount() const noexcept          { return listenerCount.load(); }
    int getPort() const noexcept                   { return port.load(); }
    juce::String getPublicUrl() const;
    TunnelState getTunnelState() const;
    bool isSourceLive() const noexcept;

private:
    void run() override;
    void handleConnection (std::unique_ptr<juce::StreamingSocket>);
    void sendHttp (juce::StreamingSocket&, const juce::String& status, const juce::String& contentType,
                   const void* body, size_t bodySize);
    juce::String statusJson() const;
    void broadcastStatus();

    juce::StreamingSocket server;
    juce::CriticalSection lock;                        // listeners + activation
    juce::OwnedArray<ListenerConnection> listeners;
    std::unique_ptr<CloudflaredTunnel> tunnel;

    std::atomic<bool> active { false };
    std::atomic<const void*> source { nullptr };
    std::atomic<int> listenerCount { 0 }, port { 0 };
    std::atomic<juce::uint32> lastPacketMs { 0 }, fmtRate { 0 };
    std::atomic<int> fmtBits { 0 };

    JUCE_DECLARE_NON_COPYABLE (StreamHub)
};
