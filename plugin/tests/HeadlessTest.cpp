// Headless end-to-end test of the all-in-one plug-in:
// drives the real MixStreamerProcessor with a stereo sine in real time, connects to the
// embedded server as a browser would (HTTP + /listen WebSocket) and verifies the stream.
//
//   MixStreamerHeadlessTest                    run the test (exit code 0 = pass)
//   MixStreamerHeadlessTest --serve <seconds>  just stream a sine (for manual/browser tests)
//   MixStreamerHeadlessTest --snapshot <png>   render the editor to a PNG
//
// Set MIXSTREAMER_NO_TUNNEL=1 to skip cloudflared, or MIXSTREAMER_CLOUDFLARED=<path>.
#include "../Source/PluginProcessor.h"
#include "../Source/Sha1.h"
#include "WebSocketClient.h"
#include <iostream>

namespace
{
    struct Driver
    {
        MixStreamerProcessor& proc;
        double sr = 48000.0, phase = 0.0;
        int block = 512, blocksDone = 0;
        bool passThroughOk = true;
        juce::AudioBuffer<float> buf { 2, 512 }, ref { 2, 512 };
        juce::MidiBuffer midi;
        double t0 = juce::Time::getMillisecondCounterHiRes();

        void processDue()
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            while (t0 + blocksDone * block * 1000.0 / sr <= now)
            {
                for (int i = 0; i < block; ++i)
                {
                    const float v = 0.5f * (float) std::sin (phase);
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, -v);
                    phase += juce::MathConstants<double>::twoPi * 1000.0 / sr;
                }
                ref.makeCopyOf (buf);
                proc.processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block; ++i)
                        if (buf.getSample (ch, i) != ref.getSample (ch, i)) passThroughOk = false;
                ++blocksDone;
            }
        }
    };

    juce::String httpGet (int port, const juce::String& path)
    {
        juce::StreamingSocket s;
        if (! s.connect ("127.0.0.1", port, 1000)) return {};
        const auto req = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
        s.write (req.toRawUTF8(), (int) req.length());
        juce::MemoryOutputStream out;
        char b[4096];
        while (s.waitUntilReady (true, 1000) == 1)
        {
            const int n = s.read (b, sizeof (b), false);
            if (n <= 0) break;
            out.write (b, (size_t) n);
        }
        return out.toString();
    }

    bool check (bool cond, const char* what)
    {
        std::cout << (cond ? "  ok   " : "  FAIL ") << what << std::endl;
        return cond;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode = argc > 1 ? argv[1] : "";

    auto proc = std::make_unique<MixStreamerProcessor>();
    proc->setPlayConfigDetails (2, 2, 48000.0, 512);
    proc->prepareToPlay (48000.0, 512);

    if (mode == "--snapshot" && argc == 3)
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        return 0;
    }

    Driver drv { *proc };
    auto& hub = proc->getHub();

    if (mode == "--serve")
    {
        const double secs = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 60.0;
        bool printedPort = false, printedUrl = false;
        while (juce::Time::getMillisecondCounterHiRes() - drv.t0 < secs * 1000.0)
        {
            drv.processDue();
            if (! printedPort && hub.getPort() > 0) { std::cout << "PORT " << hub.getPort() << std::endl; printedPort = true; }
            if (! printedUrl && hub.getPublicUrl().isNotEmpty()) { std::cout << "URL " << hub.getPublicUrl() << std::endl; printedUrl = true; }
            juce::Thread::sleep (2);
        }
        return 0;
    }

    bool ok = true;
    {
        const juce::String key = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const auto d = sha1 (key.toRawUTF8(), (size_t) key.length());
        ok &= check (juce::Base64::toBase64 (d.data(), d.size()) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "SHA-1 / RFC 6455 accept key");
    }

    // 1) nothing must start before audio has flowed for ~0.5 s (plug-in scan safety)
    ok &= check (! hub.isActive(), "hub idle before audio");
    while (! hub.isActive() && juce::Time::getMillisecondCounterHiRes() - drv.t0 < 3000)
    {
        drv.processDue();
        juce::Thread::sleep (2);
    }
    ok &= check (hub.isActive() && hub.getPort() > 0, "hub activated after audio");
    const int port = hub.getPort();
    std::cout << "  port " << port << std::endl;

    // 2) static files + status
    ok &= check (httpGet (port, "/").contains ("<html"), "GET / serves player");
    ok &= check (httpGet (port, "/worklet.js").contains ("registerProcessor"), "GET /worklet.js");
    ok &= check (httpGet (port, "/nope").startsWith ("HTTP/1.1 404"), "404 for unknown path");

    // 3) listen like a browser
    WebSocketClient ws;
    int frames = 0, gaps = 0, badFrames = 0;
    long long samples = 0;
    double peak = 0, maxErrLR = 0;
    juce::int64 lastSeq = -1;
    bool sawStatus = false;
    ws.onTextMessage = [&] (const juce::String& t) { if (t.contains ("\"status\"")) sawStatus = true; };
    ws.onBinaryMessage = [&] (const juce::uint8* d, size_t n)
    {
        if (n < 16 || memcmp (d, "MXS1", 4) != 0 || d[8] != 24 || d[9] != 2 || (n - 16) % 6 != 0) { ++badFrames; return; }
        const juce::uint32 seq = (juce::uint32) d[12] | ((juce::uint32) d[13] << 8) | ((juce::uint32) d[14] << 16) | ((juce::uint32) d[15] << 24);
        if (lastSeq >= 0 && seq != (juce::uint32) (lastSeq + 1)) ++gaps;
        lastSeq = seq;
        ++frames;
        for (size_t o = 16; o + 6 <= n; o += 6)
        {
            auto rd = [&] (size_t p) { int v = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16); if (v & 0x800000) v -= 0x1000000; return v / 8388608.0; };
            const double L = rd (o), R = rd (o + 3);
            peak = juce::jmax (peak, std::abs (L));
            maxErrLR = juce::jmax (maxErrLR, std::abs (L + R));
            ++samples;
        }
    };
    ok &= check (ws.connect ("127.0.0.1", port, "/listen", 1000), "WebSocket /listen handshake");

    const double listenStart = juce::Time::getMillisecondCounterHiRes();
    int maxListeners = 0;
    while (juce::Time::getMillisecondCounterHiRes() - listenStart < 3000)
    {
        drv.processDue();
        ws.poll();
        maxListeners = juce::jmax (maxListeners, hub.getListenerCount());
        juce::Thread::sleep (1);
    }
    std::cout << "  frames " << frames << ", samples " << samples << ", peak " << peak
              << ", |L+R| max " << maxErrLR << ", gaps " << gaps << std::endl;
    ok &= check (frames > 100 && samples > 48000 * 2.5, "audio received in real time");
    ok &= check (badFrames == 0 && gaps == 0, "headers valid, no sequence gaps");
    ok &= check (std::abs (peak - 0.5) < 1e-4 && maxErrLR == 0.0, "24-bit content bit-exact");
    ok &= check (sawStatus, "status message received");
    ok &= check (maxListeners == 1, "listener counted");
    ok &= check (drv.passThroughOk, "audio passes through unchanged");

    ws.disconnect();
    for (int i = 0; i < 100 && hub.getListenerCount() != 0; ++i) { drv.processDue(); juce::Thread::sleep (10); }
    ok &= check (hub.getListenerCount() == 0, "listener removed after disconnect");

    if (auto env = juce::SystemStats::getEnvironmentVariable ("MIXSTREAMER_CLOUDFLARED", {}); env.isNotEmpty())
    {
        for (int i = 0; i < 500 && hub.getPublicUrl().isEmpty(); ++i) { drv.processDue(); juce::Thread::sleep (10); }
        std::cout << "  url " << hub.getPublicUrl() << std::endl;
        ok &= check (hub.getPublicUrl().endsWith (".trycloudflare.com"), "tunnel URL parsed");
    }

    proc.reset();
    std::cout << (ok ? "ALL TESTS PASSED" : "TESTS FAILED") << std::endl;
    return ok ? 0 : 1;
}
