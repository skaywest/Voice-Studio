#include "PluginProcessor.h"
#include <cmath>
#include <iostream>

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << "\n";
            ++failures;
        }
        else
        {
            std::cout << "PASS: " << message << "\n";
        }
    }

    void setParameter(VoiceStudioAudioProcessor& processor, const char* id, float plainValue)
    {
        if (auto* parameter = processor.parameters.getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
    }
}

int main()
{
    VoiceStudioAudioProcessor processor;

    expect(processor.parameters.getRawParameterValue("aiEnabled")->load() < 0.5f,
           "AI is disabled by default");
    expect(processor.parameters.getRawParameterValue("dspEnabled")->load() > 0.5f,
           "DSP is enabled by default");
    expect(processor.parameters.getRawParameterValue("highPass")->load() > 0.5f,
           "high-pass is enabled by default");
    expect(processor.parameters.getRawParameterValue("presence")->load() > 0.5f,
           "presence EQ is enabled by default");
    expect(processor.parameters.getRawParameterValue("compressor")->load() > 0.5f,
           "compressor is enabled by default");
    expect(std::abs(processor.parameters.getRawParameterValue("outputGain")->load()) < 0.001f,
           "output gain defaults to 0 dB");

    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer midi;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(channel, sample, 0.25f + 0.001f * static_cast<float>(sample));

    setParameter(processor, "dspEnabled", 0.0f);
    juce::AudioBuffer<float> dryReference(buffer);
    processor.processBlock(buffer, midi);

    bool unchanged = true;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            unchanged = unchanged && buffer.getSample(channel, sample) == dryReference.getSample(channel, sample);
    expect(unchanged, "disabled DSP passes audio through unchanged");

    setParameter(processor, "dspEnabled", 1.0f);
    processor.processBlock(buffer, midi);
    bool finite = true;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            finite = finite && std::isfinite(buffer.getSample(channel, sample));
    expect(finite, "enabled DSP produces only finite samples");

    // Verify the output gain numerically with all tone/dynamics stages disabled.
    setParameter(processor, "highPass", 0.0f);
    setParameter(processor, "presence", 0.0f);
    setParameter(processor, "compressor", 0.0f);
    setParameter(processor, "outputGain", 6.0f);
    setParameter(processor, "dspEnabled", 1.0f);
    juce::AudioBuffer<float> gainBuffer(2, 2048);
    gainBuffer.clear();
    for (int channel = 0; channel < gainBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < gainBuffer.getNumSamples(); ++sample)
            gainBuffer.setSample(channel, sample, 0.1f);
    processor.processBlock(gainBuffer, midi);
    const float expectedGain = juce::Decibels::decibelsToGain(6.0f);
    bool gainCorrect = true;
    for (int channel = 0; channel < gainBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < gainBuffer.getNumSamples(); ++sample)
            gainCorrect = gainCorrect && std::abs(gainBuffer.getSample(channel, sample) - 0.1f * expectedGain) < 0.0001f;
    expect(gainCorrect, "smoothed output gain reaches the requested +6 dB without a discontinuous step");

    juce::AudioProcessor::BusesLayout monoLayout;
    monoLayout.inputBuses.add(juce::AudioChannelSet::mono());
    monoLayout.outputBuses.add(juce::AudioChannelSet::mono());
    expect(processor.isBusesLayoutSupported(monoLayout), "matching mono input/output layout is supported");

    setParameter(processor, "dspEnabled", 0.0f);
    juce::MemoryBlock state;
    processor.getStateInformation(state);

    VoiceStudioAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    expect(restored.parameters.getRawParameterValue("dspEnabled")->load() < 0.5f,
           "DSP toggle is restored from saved state");
    expect(std::abs(restored.parameters.getRawParameterValue("outputGain")->load() - 6.0f) < 0.11f,
           "output gain is restored from saved state");

    if (failures != 0)
    {
        std::cerr << failures << " processor smoke test(s) failed\n";
        return 1;
    }

    std::cout << "All processor smoke tests passed\n";
    return 0;
}
