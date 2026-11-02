#include "PluginProcessor.h"
#include "PluginEditor.h"

VoiceStudioAudioProcessor::VoiceStudioAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

VoiceStudioAudioProcessor::APVTS::ParameterLayout VoiceStudioAudioProcessor::createParameterLayout()
{
    APVTS::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterBool>("aiEnabled", "AI Voice", false));
    layout.add(std::make_unique<juce::AudioParameterBool>("dspEnabled", "DSP Effects", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "voicePreset", "Voice Preset",
        juce::StringArray { "Radio Operator", "Deep Male", "Female Voice", "Robot", "Demon", "Anime Voice" }, 0));
    return layout;
}

void VoiceStudioAudioProcessor::prepareToPlay(double, int) {}

bool VoiceStudioAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo())
        && input == output;
}

void VoiceStudioAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals guard;
    // Safe prototype: transparent pass-through. AI is not implemented or started.
    // DSP and AI switches are UI/state placeholders until their processors are implemented.
    juce::ignoreUnused(buffer);
}

juce::AudioProcessorEditor* VoiceStudioAudioProcessor::createEditor()
{
    return new VoiceStudioAudioProcessorEditor(*this);
}

void VoiceStudioAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, dest);
}

void VoiceStudioAudioProcessor::setStateInformation(const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, size));
    if (xml && xml->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoiceStudioAudioProcessor();
}
