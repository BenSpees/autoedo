#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/PsolaPitchCorrector.h"

/**
    AutoEDO — a pitch corrector that tunes to arbitrary equal divisions of the
    octave (EDO), with C fixed at its standard-tuning frequency.
*/
class AutoEdoAudioProcessor : public juce::AudioProcessor
{
public:
    AutoEdoAudioProcessor();
    ~AutoEdoAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                  { return true; }

    const juce::String getName() const override      { return "AutoEDO"; }
    bool acceptsMidi() const override                { return false; }
    bool producesMidi() const override               { return false; }
    bool isMidiEffect() const override               { return false; }
    double getTailLengthSeconds() const override     { return 0.0; }

    int getNumPrograms() override                    { return 1; }
    int getCurrentProgram() override                 { return 0; }
    void setCurrentProgram (int) override            {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Parameter IDs (shared with the editor).
    static constexpr const char* kParamEdo    = "edo";
    static constexpr const char* kParamRetune = "retune";

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    autoedo::PsolaPitchCorrector corrector;

    std::atomic<float>* edoParam    = nullptr;
    std::atomic<float>* retuneParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoEdoAudioProcessor)
};
