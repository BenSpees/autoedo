#include "PluginEditor.h"
#include "dsp/Tuning.h"
#include <cmath>

namespace
{
constexpr int kWidth  = 620;
constexpr int kHeight = 560;

const juce::Colour kBg       { 0xff1b1f2a };
const juce::Colour kPanel    { 0xff262c3b };
const juce::Colour kPanelLit { 0xff333a4d };
const juce::Colour kAccent   { 0xff5ec8d8 };
const juce::Colour kText     { 0xffe6eaf2 };
const juce::Colour kInk      { 0xff10141c };

// Nearest 12-TET note name + cents, as a familiar reference for the read-out.
juce::String note12TET (double hz)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                   "F#", "G", "G#", "A", "A#", "B" };
    const double value = 69.0 + 12.0 * std::log2 (hz / 440.0);
    const int    midi  = static_cast<int> (std::lround (value));
    const int    cents = static_cast<int> (std::lround ((value - midi) * 100.0));

    juce::String s = juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    if (cents != 0)
        s += juce::String (cents > 0 ? " +" : " ") + juce::String (cents)
           + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2")); // cent sign
    return s;
}

// Target pitch described by its EDO degree and octave (relative to C0).
juce::String degreeInfo (double hz, int edo)
{
    const long j   = std::lround (static_cast<double> (edo) * std::log2 (hz / autoedo::kReferenceC0Hz));
    const int  deg = static_cast<int> (((j % edo) + edo) % edo);
    const long oct = (j - deg) / edo;
    return "degree " + juce::String (deg) + " / " + juce::String (edo)
         + "  (oct " + juce::String (oct) + ")";
}
}

AutoEdoAudioProcessorEditor::AutoEdoAudioProcessorEditor (AutoEdoAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef (p)
{
    auto setupSlider = [this] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 100, 22);
        s.setColour (juce::Slider::textBoxTextColourId, kText);
        s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
        s.setColour (juce::Slider::rotarySliderOutlineColourId, kPanelLit);
        s.setColour (juce::Slider::thumbColourId, kAccent);
        addAndMakeVisible (s);
    };

    auto setupLabel = [this] (juce::Label& l, const juce::String& text, float h,
                              juce::Justification just = juce::Justification::centred,
                              juce::Colour colour = kText)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (just);
        l.setColour (juce::Label::textColourId, colour);
        l.setFont (juce::Font (juce::FontOptions{}.withHeight (h)));
        addAndMakeVisible (l);
    };

    setupSlider (edoSlider);
    setupSlider (retuneSlider);

    edoAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, AutoEdoAudioProcessor::kParamEdo, edoSlider);
    retuneAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, AutoEdoAudioProcessor::kParamRetune, retuneSlider);

    setupLabel (titleLabel,  "AutoEDO", 30.0f, juce::Justification::centredLeft, kAccent);
    setupLabel (infoLabel,   "", 13.0f, juce::Justification::centredLeft, kText.withAlpha (0.7f));
    setupLabel (edoLabel,    "Divisions / Octave", 15.0f);
    setupLabel (retuneLabel, "Retune Speed",       15.0f);
    setupLabel (inReadout,   "In   —",  16.0f, juce::Justification::centredLeft);
    setupLabel (outReadout,  "Out  —",  16.0f, juce::Justification::centredLeft, kAccent);
    setupLabel (degreesHeading, "Enabled scale degrees  (0 = C, the reference)", 14.0f,
                juce::Justification::centredLeft, kText.withAlpha (0.85f));

    auto setupBulk = [this] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, kPanelLit);
        b.setColour (juce::TextButton::textColourOffId, kText);
        addAndMakeVisible (b);
    };
    setupBulk (allButton);
    setupBulk (noneButton);
    allButton.onClick  = [this] { setAllDegrees (true,  static_cast<int> (edoSlider.getValue())); };
    noneButton.onClick = [this] { setAllDegrees (false, static_cast<int> (edoSlider.getValue())); };

    for (int i = 0; i < AutoEdoAudioProcessor::kNumDegrees; ++i)
    {
        auto& b = degButtons[static_cast<size_t> (i)];
        b.setClickingTogglesState (true);
        b.setButtonText (i == 0 ? "C" : juce::String (i));
        const bool root = (i == 0);
        b.setColour (juce::TextButton::buttonColourId, root ? kAccent.withAlpha (0.25f) : kPanelLit);
        b.setColour (juce::TextButton::buttonOnColourId, kAccent);
        b.setColour (juce::TextButton::textColourOffId, kText.withAlpha (0.65f));
        b.setColour (juce::TextButton::textColourOnId, kInk);
        addAndMakeVisible (b);

        degAttachments[static_cast<size_t> (i)] = std::make_unique<ButtonAttachment> (
            processorRef.apvts, AutoEdoAudioProcessor::degreeParamID (i), b);
    }

    setSize (kWidth, kHeight);
    startTimerHz (20);
    updateReadout();
}

AutoEdoAudioProcessorEditor::~AutoEdoAudioProcessorEditor()
{
    stopTimer();
}

void AutoEdoAudioProcessorEditor::setAllDegrees (bool enabled, int edo)
{
    for (int i = 0; i < edo && i < AutoEdoAudioProcessor::kNumDegrees; ++i)
        if (auto* param = processorRef.apvts.getParameter (AutoEdoAudioProcessor::degreeParamID (i)))
            param->setValueNotifyingHost (enabled ? 1.0f : 0.0f);
}

void AutoEdoAudioProcessorEditor::updateReadout()
{
    if (processorRef.isVoicedNow())
    {
        const double det = processorRef.getDetectedHz();
        const double tgt = processorRef.getTargetHz();
        const int    edo = static_cast<int> (edoSlider.getValue());

        inReadout.setText ("In    " + juce::String (det, 1) + " Hz    " + note12TET (det),
                           juce::dontSendNotification);
        outReadout.setText ("Out   " + juce::String (tgt, 1) + " Hz    " + degreeInfo (tgt, edo),
                            juce::dontSendNotification);
    }
    else
    {
        inReadout.setText  ("In    —", juce::dontSendNotification);
        outReadout.setText ("Out   —", juce::dontSendNotification);
    }
}

void AutoEdoAudioProcessorEditor::timerCallback()
{
    updateReadout();

    const int edo  = static_cast<int> (edoSlider.getValue());
    const double s = autoedo::edoStepCents (edo);
    infoLabel.setText (juce::String (edo) + "-EDO   |   step = " + juce::String (s, 2)
                       + " cents   |   C fixed at standard tuning",
                       juce::dontSendNotification);

    if (edo != shownEdo)
        resized(); // reflow the degree grid for the new division count
}

void AutoEdoAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
}

void AutoEdoAudioProcessorEditor::layoutDegreeGrid (juce::Rectangle<int> area, int edo)
{
    edo = juce::jlimit (1, AutoEdoAudioProcessor::kNumDegrees, edo);

    const int cols = juce::jmin (12, edo);
    const int rows = (edo + cols - 1) / cols;

    const int cellW = area.getWidth() / cols;
    const int cellH = juce::jmin (40, area.getHeight() / juce::jmax (1, rows));
    const int gridW = cellW * cols;
    const int startX = area.getX() + (area.getWidth() - gridW) / 2;

    for (int i = 0; i < AutoEdoAudioProcessor::kNumDegrees; ++i)
    {
        auto& b = degButtons[static_cast<size_t> (i)];
        if (i < edo)
        {
            const int row = i / cols;
            const int col = i % cols;
            b.setVisible (true);
            b.setBounds (juce::Rectangle<int> (startX + col * cellW, area.getY() + row * cellH,
                                               cellW, cellH).reduced (3));
        }
        else
        {
            b.setVisible (false);
        }
    }

    shownEdo = edo;
}

void AutoEdoAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    const int edo = static_cast<int> (edoSlider.getValue());

    titleLabel.setBounds (area.removeFromTop (40));
    infoLabel.setBounds  (area.removeFromTop (20));
    area.removeFromTop (6);

    inReadout.setBounds  (area.removeFromTop (24));
    outReadout.setBounds (area.removeFromTop (24));
    area.removeFromTop (8);

    auto knobs = area.removeFromTop (132);
    auto left  = knobs.removeFromLeft (knobs.getWidth() / 2);
    auto right = knobs;
    edoLabel.setBounds    (left.removeFromTop (22));
    retuneLabel.setBounds (right.removeFromTop (22));
    edoSlider.setBounds    (left.reduced (8));
    retuneSlider.setBounds (right.reduced (8));

    area.removeFromTop (6);
    auto heading = area.removeFromTop (28);
    noneButton.setBounds (heading.removeFromRight (60).reduced (2));
    allButton.setBounds  (heading.removeFromRight (60).reduced (2));
    degreesHeading.setBounds (heading);

    area.removeFromTop (4);
    layoutDegreeGrid (area, edo);
}
