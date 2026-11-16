// Drives the real MixStreamerProcessor with a stereo sine in real time and checks
// that it connects to the relay and reports listeners. Requires a running server.
#include "../Source/PluginProcessor.h"
#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    auto proc = std::make_unique<MixStreamerProcessor>();

    // Optional: render the editor to a PNG (scale 2) and exit.
    if (argc == 3 && juce::String (argv[1]) == "--snapshot")
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        return 0;
    }
    const double sr = 48000.0;
    const int block = 512;
    proc->setPlayConfigDetails (2, 2, sr, block);
    proc->prepareToPlay (sr, block);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    double phase = 0.0;
    bool passThroughOk = true, everConnected = false;
    int maxListeners = 0;

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int b = 0; b < (int) (sr * 6 / block); ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const float v = 0.5f * (float) std::sin (phase);
            buf.setSample (0, i, v);
            buf.setSample (1, i, -v);
            phase += juce::MathConstants<double>::twoPi * 1000.0 / sr;
        }
        juce::AudioBuffer<float> ref; ref.makeCopyOf (buf);
        proc->processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
                if (buf.getSample (ch, i) != ref.getSample (ch, i)) passThroughOk = false;

        everConnected |= proc->getSender().isConnected();
        maxListeners = juce::jmax (maxListeners, proc->getSender().getListenerCount());

        const double due = t0 + (b + 1) * block * 1000.0 / sr;
        while (juce::Time::getMillisecondCounterHiRes() < due) juce::Thread::sleep (1);
    }

    proc->releaseResources();
    proc.reset();
    std::cout << "passThrough=" << passThroughOk << " connected=" << everConnected
              << " maxListeners=" << maxListeners << std::endl;
    return (passThroughOk && everConnected && maxListeners >= 1) ? 0 : 1;
}
