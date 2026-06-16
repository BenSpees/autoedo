#pragma once

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

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    AutoEdoAudioProcessor& processorRef;

    juce::Slider edoSlider;
    juce::Slider retuneSlider;
    juce::Label  edoLabel, retuneLabel, titleLabel, infoLabel;

    std::unique_ptr<SliderAttachment> edoAttachment;
    std::unique_ptr<SliderAttachment> retuneAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoEdoAudioProcessorEditor)
};
