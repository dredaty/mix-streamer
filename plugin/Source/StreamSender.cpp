#include "StreamSender.h"
#include <cmath>

namespace
{
    constexpr int headerSize = 16;
    constexpr int maxFramesPerPacket = 4096;

    inline void putU32 (juce::uint8* p, juce::uint32 v) noexcept
    {
        p[0] = (juce::uint8) v; p[1] = (juce::uint8) (v >> 8);
        p[2] = (juce::uint8) (v >> 16); p[3] = (juce::uint8) (v >> 24);
    }
}

StreamSender::StreamSender (StreamHub& h) : juce::Thread ("MixStreamer Sender"), hub (h)
{
    fifoData.resize ((size_t) fifoCapacity * 2, 0.0f);
    packet.resize (headerSize + (size_t) maxFramesPerPacket * 2 * 3);
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
    hub.releaseSource (this);
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
    juce::uint32 audioSince = 0;

    while (! threadShouldExit())
    {
        const int sr = (int) sampleRate.load();
        const int chunk = juce::jlimit (64, maxFramesPerPacket, sr / 100);  // ~10 ms packets
        const int ready = fifo.getNumReady();

        // Activate server + tunnel only after ~0.5 s of continuous audio, so that
        // plug-in scans / short instantiations never start anything.
        if (! hub.isActive())
        {
            if (ready > 0)
            {
                if (audioSince == 0) audioSince = juce::Time::getMillisecondCounter();
                if (juce::Time::getMillisecondCounter() - audioSince > 500)
                    hub.activate();
            }
            fifo.read (ready);  // discard
            wait (20);
            continue;
        }

        if (! hub.isSource (this) && ! hub.claimSource (this))
        {
            fifo.read (ready);  // another instance is streaming
            wait (50);
            continue;
        }

        if (ready >= chunk)
            sendAvailable (juce::jmin (ready, maxFramesPerPacket));
        else
            wait (2);
    }
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
    hub.publish (this, std::make_shared<const juce::MemoryBlock> (p, total), sampleRate.load(), bits);
}
