#include "PluginEditor.h"
#include "dsp/Tuning.h"

namespace
{
constexpr int kWidth  = 420;
constexpr int kHeight = 300;

const juce::Colour kBg     { 0xff1b1f2a };
const juce::Colour kPanel  { 0xff262c3b };
const juce::Colour kAccent { 0xff5ec8d8 };
const juce::Colour kText   { 0xffe6eaf2 };
}

AutoEdoAudioProcessorEditor::AutoEdoAudioProcessorEditor (AutoEdoAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef (p)
{
    auto setupSlider = [this] (juce::Slider& s, juce::Slider::SliderStyle style)
    {
        s.setSliderStyle (style);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 22);
        s.setColour (juce::Slider::textBoxTextColourId, kText);
        s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
        s.setColour (juce::Slider::thumbColourId, kAccent);
        s.setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.5f));
        addAndMakeVisible (s);
    };

    auto setupLabel = [this] (juce::Label& l, const juce::String& text, float fontHeight,
                              juce::Justification just = juce::Justification::centred)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (just);
        l.setColour (juce::Label::textColourId, kText);
        l.setFont (juce::Font (juce::FontOptions{}.withHeight (fontHeight)));
        addAndMakeVisible (l);
    };

    setupSlider (edoSlider,    juce::Slider::RotaryHorizontalVerticalDrag);
    setupSlider (retuneSlider, juce::Slider::RotaryHorizontalVerticalDrag);

    edoAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, AutoEdoAudioProcessor::kParamEdo, edoSlider);
    retuneAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, AutoEdoAudioProcessor::kParamRetune, retuneSlider);

    setupLabel (titleLabel,  "AutoEDO", 30.0f);
    titleLabel.setColour (juce::Label::textColourId, kAccent);
    setupLabel (edoLabel,    "Divisions / Octave", 15.0f);
    setupLabel (retuneLabel, "Retune Speed",       15.0f);
    setupLabel (infoLabel,   "", 13.0f);
    infoLabel.setColour (juce::Label::textColourId, kText.withAlpha (0.7f));

    setSize (kWidth, kHeight);
    startTimerHz (15);
    timerCallback();
}

AutoEdoAudioProcessorEditor::~AutoEdoAudioProcessorEditor()
{
    stopTimer();
}

void AutoEdoAudioProcessorEditor::timerCallback()
{
    const int edo = static_cast<int> (processorRef.apvts
                        .getRawParameterValue (AutoEdoAudioProcessor::kParamEdo)->load());

    const double step = autoedo::edoStepCents (edo);
    infoLabel.setText (juce::String (edo) + "-EDO  |  step = "
                       + juce::String (step, 2) + " cents  |  C = reference (standard tuning)",
                       juce::dontSendNotification);
}

void AutoEdoAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    auto bounds = getLocalBounds().toFloat().reduced (12.0f);
    bounds.removeFromTop (54.0f); // leave room for the title
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds, 10.0f);
}

void AutoEdoAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);

    titleLabel.setBounds (area.removeFromTop (44));

    auto footer = area.removeFromBottom (34);
    infoLabel.setBounds (footer);

    area.removeFromTop (8);

    auto knobs = area;
    const int colW = knobs.getWidth() / 2;

    auto left  = knobs.removeFromLeft (colW);
    auto right = knobs;

    edoLabel.setBounds   (left.removeFromTop (24));
    retuneLabel.setBounds (right.removeFromTop (24));

    edoSlider.setBounds   (left.reduced (10));
    retuneSlider.setBounds (right.reduced (10));
}
