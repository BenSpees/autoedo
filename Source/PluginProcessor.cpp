#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Tuning.h"

AutoEdoAudioProcessor::AutoEdoAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    edoParam    = apvts.getRawParameterValue (kParamEdo);
    retuneParam = apvts.getRawParameterValue (kParamRetune);
}

juce::AudioProcessorValueTreeState::ParameterLayout
AutoEdoAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { kParamEdo, 1 }, "EDO",
        autoedo::kMinEdo, autoedo::kMaxEdo, 12,
        AudioParameterIntAttributes().withStringFromValueFunction (
            [] (int v, int) { return String (v) + (v == 12 ? " (12-TET)" : ""); })));

    // Retune speed in milliseconds: 0 ms = hard snap, higher = gentle glide.
    // Skewed so the musically useful fast end has more travel.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { kParamRetune, 1 }, "Retune Speed",
        NormalisableRange<float> (0.0f, 500.0f, 0.1f, 0.4f), 20.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                return v < 1.0f ? String ("Hard") : String (v, 0) + " ms";
            })));

    return layout;
}

void AutoEdoAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int numCh = juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels(), 1);
    corrector.prepare (sampleRate, numCh, samplesPerBlock);
    setLatencySamples (corrector.getLatencySamples());
}

bool AutoEdoAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out)
        return false;

    return out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::stereo();
}

void AutoEdoAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channels with no corresponding input.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    corrector.setEdo (static_cast<int> (edoParam->load()));
    corrector.setRetuneMs (static_cast<double> (retuneParam->load()));

    corrector.process (buffer.getArrayOfWritePointers(),
                       buffer.getNumChannels(),
                       buffer.getNumSamples());
}

juce::AudioProcessorEditor* AutoEdoAudioProcessor::createEditor()
{
    return new AutoEdoAudioProcessorEditor (*this);
}

void AutoEdoAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AutoEdoAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutoEdoAudioProcessor();
}
