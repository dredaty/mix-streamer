#include "StreamSender.h"
#include <cmath>

namespace
{
    constexpr int headerSize = 16;
    constexpr int maxFramesPerPacket = 4096;
    constexpr juce::uint32 reconnectIntervalMs = 1000;

    inline void putU32 (juce::uint8* p, juce::uint32 v) noexcept
    {
        p[0] = (juce::uint8) v; p[1] = (juce::uint8) (v >> 8);
        p[2] = (juce::uint8) (v >> 16); p[3] = (juce::uint8) (v >> 24);
    }
}

StreamSender::StreamSender() : juce::Thread ("MixStreamer Sender")
{
    fifoData.resize ((size_t) fifoCapacity * 2, 0.0f);
    packet.resize (headerSize + (size_t) maxFramesPerPacket * 2 * 3);

    ws.onTextMessage = [this] (const juce::String& text)
    {
        const auto v = juce::JSON::parse (text);
        if (v.getProperty ("type", {}).toString() == "listeners")
            listeners.store ((int) v.getProperty ("count", 0));
    };
}

StreamSender::~StreamSender() { stop(); }

void StreamSender::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::normal);
}

void StreamSender::stop()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);
}

void StreamSender::pushStereo (const float* left, const float* right, int numSamples) noexcept
{
    const auto scope = fifo.write (juce::jmin (numSamples, fifo.getFreeSpace()));
    float* d = fifoData.data();

    for (int i = 0; i < scope.blockSize1; ++i)
    {
        d[2 * (scope.startIndex1 + i)]     = left[i];
        d[2 * (scope.startIndex1 + i) + 1] = right[i];
    }
    for (int i = 0; i < scope.blockSize2; ++i)
    {
        const int s = scope.blockSize1 + i;
        d[2 * (scope.startIndex2 + i)]     = left[s];
        d[2 * (scope.startIndex2 + i) + 1] = right[s];
    }
}

void StreamSender::run()
{
    juce::uint32 lastAttempt = 0;

    while (! threadShouldExit())
    {
        if (! ws.isConnected())
        {
            connected.store (false);
            listeners.store (0);

            // Discard audio while offline so no stale backlog builds up.
            fifo.read (fifo.getNumReady());

            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastAttempt >= reconnectIntervalMs || lastAttempt == 0)
            {
                lastAttempt = now;
                if (ws.connect (host, port, "/ingest", 300))
                {
                    fifo.read (fifo.getNumReady()); // start fresh
                    connected.store (true);
                    continue;
                }
            }
            wait (50);
            continue;
        }

        if (! ws.poll())
            continue;

        // ~10 ms per packet: good balance between latency and overhead.
        const int sr = (int) sampleRate.load();
        const int chunk = juce::jlimit (64, maxFramesPerPacket, sr / 100);
        const int ready = fifo.getNumReady();

        if (ready >= chunk)
            sendAvailable (juce::jmin (ready, maxFramesPerPacket));
        else
            wait (2);
    }

    ws.disconnect();
    connected.store (false);
}

void StreamSender::sendAvailable (int frames)
{
    const int bits = bitDepth.load();
    const int bytesPerSample = bits / 8;
    auto* p = packet.data();

    // header
    p[0] = 'M'; p[1] = 'X'; p[2] = 'S'; p[3] = '1';
    putU32 (p + 4, sampleRate.load());
    p[8] = (juce::uint8) bits;
    p[9] = 2;
    p[10] = p[11] = 0;
    putU32 (p + 12, sequence++);

    auto* out = p + headerSize;
    const auto scope = fifo.read (frames);
    const float* d = fifoData.data();

    auto convert = [&] (int start, int count)
    {
        for (int i = 0; i < count * 2; ++i)
        {
            float x = d[2 * start + i];
            if (! std::isfinite (x)) x = 0.0f;

            if (bits == 16)
            {
                // TPDF dither, ±1 LSB
                auto rnd = [this]() noexcept
                {
                    rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
                    return (float) rngState * (1.0f / 4294967296.0f);
                };
                float v = x * 32767.0f + (rnd() - rnd());
                const int s = juce::jlimit (-32768, 32767, (int) std::lrint (v));
                *out++ = (juce::uint8) s;
                *out++ = (juce::uint8) (s >> 8);
            }
            else
            {
                const int s = juce::jlimit (-8388608, 8388607, (int) std::lrint ((double) x * 8388607.0));
                *out++ = (juce::uint8) s;
                *out++ = (juce::uint8) (s >> 8);
                *out++ = (juce::uint8) (s >> 16);
            }
        }
    };

    convert (scope.startIndex1, scope.blockSize1);
    convert (scope.startIndex2, scope.blockSize2);

    const size_t total = headerSize + (size_t) frames * 2 * (size_t) bytesPerSample;
    if (! ws.sendBinary (p, total))
        connected.store (false);
}
