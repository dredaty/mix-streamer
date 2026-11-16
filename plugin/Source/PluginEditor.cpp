#include "PluginEditor.h"

namespace Colours_
{
    const juce::Colour bg      { 0xff0e1014 };
    const juce::Colour panel   { 0xff161a21 };
    const juce::Colour panel2  { 0xff1d222b };
    const juce::Colour line    { 0xff262c37 };
    const juce::Colour text    { 0xffe7eaf0 };
    const juce::Colour dim     { 0xff8a93a3 };
    const juce::Colour accent  { 0xff33d17a };
    const juce::Colour err     { 0xffff5d5d };
}
using namespace Colours_;

//============================================================================== LookAndFeel
DarkLookAndFeel::DarkLookAndFeel()
{
    setColour (juce::TextButton::buttonColourId, panel2);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, dim);
    setColour (juce::TextButton::textColourOnId, juce::Colour (0xff06210f));
    setColour (juce::Label::textColourId, text);
}

void DarkLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base,
                                            bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto c = base;
    if (down)      c = c.brighter (0.15f);
    else if (over) c = c.brighter (0.07f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 8.0f);
    if (! b.getToggleState())
    {
        g.setColour (line);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
    }
}

juce::Font DarkLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
{
    return juce::Font (juce::FontOptions (juce::jmin (15.0f, h * 0.45f), juce::Font::bold));
}

//============================================================================== Editor
MixStreamerEditor::MixStreamerEditor (MixStreamerProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);

    for (auto* b : { &btn16, &btn24 })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (b);
    }
    btn16.setConnectedEdges (juce::Button::ConnectedOnRight);
    btn24.setConnectedEdges (juce::Button::ConnectedOnLeft);
    btn16.onClick = [this] { setQuality (0); };
    btn24.onClick = [this] { setQuality (1); };

    copyBtn.setColour (juce::TextButton::buttonColourId, accent);
    copyBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff06210f));
    copyBtn.onClick = [this] { copyLink(); };
    addAndMakeVisible (copyBtn);

    linkLabel.setFont (juce::FontOptions (12.0f));
    linkLabel.setColour (juce::Label::textColourId, dim);
    linkLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (linkLabel);

    feedbackLabel.setFont (juce::FontOptions (12.0f));
    feedbackLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (feedbackLabel);

    setSize (380, 260);
    timerCallback();
    startTimerHz (10);
}

MixStreamerEditor::~MixStreamerEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

juce::File MixStreamerEditor::urlFile()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".mix-streamer-url");
}

void MixStreamerEditor::setQuality (int index)
{
    if (auto* param = proc.apvts.getParameter (MixStreamerProcessor::qualityParamId))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
        param->endChangeGesture();
    }
    timerCallback();
}

void MixStreamerEditor::copyLink()
{
    const auto url = urlFile().loadFileAsString().trim();
    if (url.startsWithIgnoreCase ("http"))
    {
        juce::SystemClipboard::copyTextToClipboard (url);
        feedbackLabel.setColour (juce::Label::textColourId, accent);
        feedbackLabel.setText ("Link copied to clipboard", juce::dontSendNotification);
    }
    else
    {
        feedbackLabel.setColour (juce::Label::textColourId, err);
        feedbackLabel.setText ("No URL yet - is the companion server running?", juce::dontSendNotification);
    }
    feedbackUntil = juce::Time::getMillisecondCounter() + 3000;
}

void MixStreamerEditor::timerCallback()
{
    auto& s = proc.getSender();
    const bool c = s.isConnected();
    const int l = s.getListenerCount();
    if (c != connected || l != listeners)
    {
        connected = c;
        listeners = l;
        repaint();
    }

    const bool is16 = proc.apvts.getRawParameterValue (MixStreamerProcessor::qualityParamId)->load() < 0.5f;
    btn16.setToggleState (is16, juce::dontSendNotification);
    btn24.setToggleState (! is16, juce::dontSendNotification);

    // Poll the URL file at ~1 Hz (cheap, message thread only).
    if (urlPollCounter++ % 10 == 0)
    {
        const auto url = urlFile().loadFileAsString().trim();
        linkLabel.setText (url.isNotEmpty() ? url : juce::String ("(no public URL yet)"), juce::dontSendNotification);
    }

    if (feedbackUntil != 0 && juce::Time::getMillisecondCounter() > feedbackUntil)
    {
        feedbackUntil = 0;
        feedbackLabel.setText ({}, juce::dontSendNotification);
    }
}

void MixStreamerEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    auto area = getLocalBounds().reduced (16);

    // Title
    auto top = area.removeFromTop (28);
    g.setColour (text);
    g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    g.drawText ("MIX STREAMER", top, juce::Justification::centredLeft);

    // Status card
    area.removeFromTop (10);
    auto card = area.removeFromTop (58).toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (card, 12.0f);
    g.setColour (line);
    g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, 1.0f);

    auto inner = card.reduced (16.0f, 0.0f);
    const auto dotCol = connected ? accent : err;
    auto dot = juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ inner.getX() + 5.0f, inner.getCentreY() });
    if (connected)
    {
        g.setColour (dotCol.withAlpha (0.25f));
        g.fillEllipse (dot.expanded (4.0f));
    }
    g.setColour (dotCol);
    g.fillEllipse (dot);

    g.setColour (text);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (connected ? "Connected" : "Disconnected",
                inner.withTrimmedLeft (22.0f).toNearestInt(), juce::Justification::centredLeft);

    g.setColour (connected ? text : dim);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    auto countArea = inner.removeFromRight (90.0f);
    g.drawText (connected ? juce::String (listeners) : "-", countArea.removeFromTop (card.getHeight() * 0.62f).toNearestInt(),
                juce::Justification::bottomRight);
    g.setColour (dim);
    g.setFont (juce::FontOptions (10.5f));
    g.drawText (listeners == 1 ? "LISTENER" : "LISTENERS", countArea.toNearestInt(), juce::Justification::topRight);

    // Section label
    g.setColour (dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("STREAM QUALITY", getLocalBounds().reduced (16).withTop (16 + 28 + 10 + 58 + 12).withHeight (14),
                juce::Justification::centredLeft);
}

void MixStreamerEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (28 + 10 + 58 + 12 + 14 + 6);

    auto row = area.removeFromTop (36);
    btn16.setBounds (row.removeFromLeft (row.getWidth() / 2));
    btn24.setBounds (row);

    area.removeFromTop (12);
    auto linkRow = area.removeFromTop (36);
    copyBtn.setBounds (linkRow.removeFromRight (120));
    linkRow.removeFromRight (8);
    linkLabel.setBounds (linkRow);

    feedbackLabel.setBounds (area.removeFromTop (22));
}
