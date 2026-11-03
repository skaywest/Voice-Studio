#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <chrono>
#include <limits>

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 # endif
 #include <windows.h>
#endif

namespace
{
std::uint64_t monotonicNowNs() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetTickCount64()) * 1000000ull;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
}

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
    preparedSampleRate = static_cast<std::uint32_t>(juce::jlimit(8000.0, 192000.0, sampleRate));
    monotonicAnchorNs = monotonicNowNs();
    audioFramesProcessed = 0;
    nextAudioSequence = 0;
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
    const auto frames = static_cast<std::uint32_t>(juce::jmax(0, buffer.getNumSamples()));

    // AI transport is opt-in and requires explicit READY/PROCESSING confirmation.
    // This callback never starts processes, performs IPC, allocates, or waits.
    if (parameters.getRawParameterValue("aiEnabled")->load() > 0.5f &&
        aiConversionAvailable.load(std::memory_order_acquire) &&
        preparedSampleRate >= 8000 && preparedSampleRate <= 192000 &&
        frames > 0 && frames <= voice_studio::ipc::kAudioBlockMaxFrames &&
        channels >= 1 && isRealtime())
    {
        const auto generation = aiGeneration.load(std::memory_order_acquire);
        if (generation != 0)
        {
            const auto wholeSeconds = audioFramesProcessed / preparedSampleRate;
            const auto remainderFrames = audioFramesProcessed % preparedSampleRate;
            const auto elapsedNs = wholeSeconds * 1000000000ull +
                (remainderFrames * 1000000000ull) / preparedSampleRate;
            const auto nowNs = monotonicAnchorNs + elapsedNs;
            const auto maxNs = (std::numeric_limits<std::uint64_t>::max)();
            const auto deadlineNs = nowNs <= maxNs - 100000000ull
                ? nowNs + 100000000ull : maxNs;
            const auto sequence = ++nextAudioSequence;
            const auto expectedSequence = sequence > 1 ? sequence - 1 : 0;
            realtimeBridge.process(realtimeAccess,
                buffer.getReadPointer(0), channels > 1 ? buffer.getReadPointer(1) : nullptr,
                buffer.getWritePointer(0), channels > 1 ? buffer.getWritePointer(1) : nullptr,
                frames, static_cast<std::uint32_t>(channels), preparedSampleRate,
                sequence, expectedSequence, generation, deadlineNs, nowNs);
        }
    }
    audioFramesProcessed += frames;

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

    // Advance the gain smoother once per frame, not once per channel.
    // This keeps the ramp duration sample-accurate and stereo channels linked.
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float gain = outputGainSmoother.getNextValue();
        for (int ch = 0; ch < channels; ++ch)
        {
            float sample = buffer.getSample(ch, i);
            auto& hp = highPassFilters[static_cast<size_t>(ch)];
            auto& eq = presenceFilters[static_cast<size_t>(ch)];
            auto& comp = compressors[static_cast<size_t>(ch)];

            if (useHighPass)
                sample = hp.processSample(sample);
            if (usePresence)
                sample = eq.processSample(sample);
            if (useCompressor)
                sample = comp.processSample(0, sample);
            buffer.setSample(ch, i, sample * gain);
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

void VoiceStudioAudioProcessor::setAiConversionAvailable(bool available, std::uint64_t generation) noexcept
{
    if (!available || generation == 0)
    {
        aiConversionAvailable.store(false, std::memory_order_release);
        aiGeneration.store(0, std::memory_order_release);
        return;
    }

    // Publish generation before availability.
    aiGeneration.store(generation, std::memory_order_release);
    aiConversionAvailable.store(true, std::memory_order_release);
}
