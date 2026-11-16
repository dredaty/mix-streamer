#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StreamSender.h"

class MixStreamerProcessor : public juce::AudioProcessor
{
public:
    MixStreamerProcessor();
    ~MixStreamerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    StreamSender& getSender() noexcept { return sender; }

    static constexpr const char* qualityParamId = "quality";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    StreamSender sender;
    std::atomic<float>* qualityParam = nullptr;   // 0 = 16-bit, 1 = 24-bit

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixStreamerProcessor)
};
