#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "WebSocketClient.h"
#include <atomic>
#include <vector>

/**
 * Lock-free SPSC bridge between the audio thread and a background network thread.
 *
 * Audio thread:  pushStereo()   – no locks, no allocation, no system calls.
 * Sender thread: drains the FIFO, converts to 16/24-bit PCM (16-bit with TPDF dither),
 *                prefixes the 16-byte header and sends it to ws://127.0.0.1:8765/ingest.
 *                Reconnects automatically every second while the server is down.
 */
class StreamSender : private juce::Thread
{
public:
    StreamSender();
    ~StreamSender() override;

    void start();
    void stop();

    void setSampleRate (double sr) noexcept    { sampleRate.store ((juce::uint32) juce::roundToInt (sr)); }
    void setBitDepth (int bits) noexcept       { bitDepth.store (bits == 16 ? 16 : 24); }

    /** Real-time safe. Drops samples if the FIFO is full. */
    void pushStereo (const float* left, const float* right, int numSamples) noexcept;

    bool isConnected() const noexcept          { return connected.load(); }
    int  getListenerCount() const noexcept     { return listeners.load(); }

    static constexpr const char* host = "127.0.0.1";
    static constexpr int port = 8765;

private:
    void run() override;
    void sendAvailable (int frames);

    // ~2.7 s at 192 kHz, 10.9 s at 48 kHz
    static constexpr int fifoCapacity = 1 << 19;
    juce::AbstractFifo fifo { fifoCapacity };
    std::vector<float> fifoData;                 // interleaved L/R, size 2 * capacity

    std::atomic<juce::uint32> sampleRate { 48000 };
    std::atomic<int> bitDepth { 24 };
    std::atomic<bool> connected { false };
    std::atomic<int> listeners { 0 };

    // sender-thread-only state
    WebSocketClient ws;
    std::vector<juce::uint8> packet;
    juce::uint32 sequence = 0;
    juce::uint32 rngState = 0x12345678;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamSender)
};
