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
    layout.add(std::make_unique<juce::AudioParameterBool>("dspEnabled", "DSP Effects", true));
    layout.add(std::make_unique<juce::AudioParameterBool>("highPass", "High-pass Filter", true));
    layout.add(std::make_unique<juce::AudioParameterBool>("presence", "Presence EQ", true));
    layout.add(std::make_unique<juce::AudioParameterBool>("compressor", "Compressor", true));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "outputGain", "Output Level", juce::NormalisableRange<float>(-18.0f, 12.0f, 0.1f), 0.0f));
    return layout;
}

void VoiceStudioAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(juce::jmax(1, samplesPerBlock));
    spec.numChannels = 1;

    outputGainSmoother.reset(sampleRate, 0.020);
    outputGainSmoother.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("outputGain")->load()));

    const auto hpCoefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 80.0f);
    const auto presenceCoefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        sampleRate, 3200.0f, 0.9f, juce::Decibels::decibelsToGain(2.5f));

    for (int ch = 0; ch < 2; ++ch)
    {
        highPassFilters[static_cast<size_t>(ch)].reset();
        presenceFilters[static_cast<size_t>(ch)].reset();
        highPassFilters[static_cast<size_t>(ch)].prepare(spec);
        presenceFilters[static_cast<size_t>(ch)].prepare(spec);
        highPassFilters[static_cast<size_t>(ch)].coefficients = hpCoefficients;
        presenceFilters[static_cast<size_t>(ch)].coefficients = presenceCoefficients;
        compressors[static_cast<size_t>(ch)].reset();
        compressors[static_cast<size_t>(ch)].prepare(spec);
        compressors[static_cast<size_t>(ch)].setThreshold(-18.0f);
        compressors[static_cast<size_t>(ch)].setRatio(3.0f);
        compressors[static_cast<size_t>(ch)].setAttack(8.0f);
        compressors[static_cast<size_t>(ch)].setRelease(100.0f);
    }
}

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
    const int channels = juce::jmin(buffer.getNumChannels(), 2);

    if (!parameters.getRawParameterValue("dspEnabled")->load())
    {
        // Keep bypass bit-transparent and synchronize the smoother so enabling
        // DSP later does not ramp from an obsolete gain value.
        outputGainSmoother.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
            parameters.getRawParameterValue("outputGain")->load()));
        return;
    }

    const bool useHighPass = parameters.getRawParameterValue("highPass")->load() > 0.5f;
    const bool usePresence = parameters.getRawParameterValue("presence")->load() > 0.5f;
    const bool useCompressor = parameters.getRawParameterValue("compressor")->load() > 0.5f;
    const float targetGain = juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("outputGain")->load());
    outputGainSmoother.setTargetValue(targetGain);

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& hp = highPassFilters[static_cast<size_t>(ch)];
        auto& eq = presenceFilters[static_cast<size_t>(ch)];
        auto& comp = compressors[static_cast<size_t>(ch)];

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float sample = data[i];
            if (useHighPass)
                sample = hp.processSample(sample);
            if (usePresence)
                sample = eq.processSample(sample);
            if (useCompressor)
                sample = comp.processSample(0, sample);
            data[i] = sample * outputGainSmoother.getNextValue();
        }
    }
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
