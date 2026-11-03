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
    expect(!processor.isAiConversionAvailable(),
           "AI conversion is unavailable until a Worker confirms processing");
#if defined(_WIN32)
    expect(processor.workerStatusText() == "WORKER STOPPED",
           "Worker UI snapshot starts in the stopped state");
    expect(!processor.launchWorker(juce::File{}),
           "invalid Worker path is rejected without queueing a launch");
#endif
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

    // Merely requesting AI must never activate an uninitialised Worker.
    setParameter(processor, "aiEnabled", 1.0f);
    processor.setAiConversionAvailable(false);
    juce::AudioBuffer<float> aiFallback(buffer);
    juce::AudioBuffer<float> aiFallbackReference(buffer);
    processor.processBlock(aiFallback, midi);
    bool aiFallbackDry = true;
    for (int channel = 0; channel < aiFallback.getNumChannels(); ++channel)
        for (int sample = 0; sample < aiFallback.getNumSamples(); ++sample)
            aiFallbackDry = aiFallbackDry && aiFallback.getSample(channel, sample) ==
                aiFallbackReference.getSample(channel, sample);
    expect(aiFallbackDry, "AI request without confirmed Worker processing remains dry");

    // An availability flag without a mapped region must also fall back safely.
    processor.setAiConversionAvailable(true, 7);
    juce::AudioBuffer<float> missingMapping(buffer);
    juce::AudioBuffer<float> missingMappingReference(buffer);
    processor.processBlock(missingMapping, midi);
    bool missingMappingDry = true;
    for (int channel = 0; channel < missingMapping.getNumChannels(); ++channel)
        for (int sample = 0; sample < missingMapping.getNumSamples(); ++sample)
            missingMappingDry = missingMappingDry && missingMapping.getSample(channel, sample) ==
                missingMappingReference.getSample(channel, sample);
    expect(missingMappingDry, "missing realtime mapping safely falls back to dry audio");
    processor.setAiConversionAvailable(false);

    setParameter(processor, "dspEnabled", 1.0f);
    processor.processBlock(buffer, midi);
    bool finite = true;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            finite = finite && std::isfinite(buffer.getSample(channel, sample));
    expect(finite, "enabled DSP produces only finite samples");

    // Verify that gain ramps smoothly and reaches its target after the 20 ms ramp.
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
    const float expectedOutput = 0.1f * expectedGain;
    bool gainRampSmooth = true;
    for (int sample = 1; sample < gainBuffer.getNumSamples(); ++sample)
    {
        const float previous = gainBuffer.getSample(0, sample - 1);
        const float current = gainBuffer.getSample(0, sample);
        gainRampSmooth = gainRampSmooth && current + 0.000001f >= previous;
        gainRampSmooth = gainRampSmooth && std::abs(current - previous) < 0.001f;
    }
    expect(gainRampSmooth, "output gain follows a smooth ramp without a discontinuous step");

    bool gainReachedTarget = true;
    for (int sample = 1024; sample < gainBuffer.getNumSamples(); ++sample)
        gainReachedTarget = gainReachedTarget &&
            std::abs(gainBuffer.getSample(0, sample) - expectedOutput) < 0.0001f;
    expect(gainReachedTarget, "smoothed output gain reaches the requested +6 dB target");

    bool stereoGainLinked = true;
    for (int sample = 0; sample < gainBuffer.getNumSamples(); ++sample)
        stereoGainLinked = stereoGainLinked &&
            std::abs(gainBuffer.getSample(0, sample) - gainBuffer.getSample(1, sample)) < 0.000001f;
    expect(stereoGainLinked, "stereo channels receive the same gain ramp on each frame");

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
