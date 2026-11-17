#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class DarkLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DarkLookAndFeel();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool isMouseOver, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

class MixStreamerEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit MixStreamerEditor (MixStreamerProcessor&);
    ~MixStreamerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setQuality (int index);
    void copyLink();

    MixStreamerProcessor& proc;
    DarkLookAndFeel lnf;
    std::unique_ptr<juce::Drawable> logo;

    juce::TextButton btn16 { "16-bit" }, btn24 { "24-bit" }, copyBtn { "Copy Link" };
    juce::Label linkLabel, feedbackLabel;

    juce::String statusText;
    juce::Colour statusColour;
    int listeners = -1;
    juce::uint32 feedbackUntil = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixStreamerEditor)
};
