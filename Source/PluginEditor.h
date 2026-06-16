#pragma once

#include <array>
#include <memory>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class AutoEdoAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit AutoEdoAudioProcessorEditor (AutoEdoAudioProcessor&);
    ~AutoEdoAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void layoutDegreeGrid (juce::Rectangle<int> area, int edo);
    void setAllDegrees (bool enabled, int edo);
    void updateReadout();

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    AutoEdoAudioProcessor& processorRef;

    juce::Slider edoSlider;
    juce::Slider retuneSlider;
    juce::Label  edoLabel, retuneLabel, titleLabel, infoLabel;
    juce::Label  inReadout, outReadout, degreesHeading;

    std::unique_ptr<SliderAttachment> edoAttachment;
    std::unique_ptr<SliderAttachment> retuneAttachment;

    std::array<juce::TextButton, AutoEdoAudioProcessor::kNumDegrees>            degButtons;
    std::array<std::unique_ptr<ButtonAttachment>, AutoEdoAudioProcessor::kNumDegrees> degAttachments;
    juce::TextButton allButton { "All" }, noneButton { "None" };

    int shownEdo = -1; // last EDO the grid was laid out for

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoEdoAudioProcessorEditor)
};
