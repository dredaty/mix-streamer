#include "PluginProcessor.h"
#include "PluginEditor.h"

MixStreamerProcessor::MixStreamerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MixStreamer", createLayout())
{
    qualityParam = apvts.getRawParameterValue (qualityParamId);
    sender.setBitDepth (qualityParam->load() < 0.5f ? 16 : 24);
    sender.start();
}

MixStreamerProcessor::~MixStreamerProcessor()
{
    sender.stop();
}

juce::AudioProcessorValueTreeState::ParameterLayout MixStreamerProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { qualityParamId, 1 }, "Stream Quality",
        juce::StringArray { "16-bit PCM", "24-bit PCM" }, 0));   // 16-bit default: safer on weak connections
    return layout;
}

void MixStreamerProcessor::prepareToPlay (double sampleRate, int)
{
    sender.setSampleRate (sampleRate);
}

bool MixStreamerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MixStreamerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Audio is passed through untouched (in-place buffer). We only copy it into the
    // lock-free FIFO: no allocation, no locks, no I/O on this thread.
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2 && n > 0)
    {
        sender.setBitDepth (qualityParam->load() < 0.5f ? 16 : 24);
        sender.pushStereo (buffer.getReadPointer (0), buffer.getReadPointer (1), n);
    }
}

juce::AudioProcessorEditor* MixStreamerProcessor::createEditor()
{
    return new MixStreamerEditor (*this);
}

void MixStreamerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MixStreamerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MixStreamerProcessor();
}
