#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "AudioIpc/AudioBlockRealtimeAccess.h"
#include "AudioIpc/RvcAudioStreamAdapter.h"
#if defined(_WIN32)
 #include "AudioIpc/RvcAudioTransportPump.h"
#endif
#if defined(_WIN32)
 #include "AudioIpc/WindowsWorkerController.h"
#endif
#include <array>
#include <atomic>
#include <cstdint>
#if defined(_WIN32)
 #include <condition_variable>
 #include <mutex>
 #include <string>
 #include <thread>
#endif

class VoiceStudioAudioProcessor final : public juce::AudioProcessor
{
public:
    VoiceStudioAudioProcessor();
    ~VoiceStudioAudioProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Voice Studio"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    using APVTS = juce::AudioProcessorValueTreeState;
    APVTS parameters;

    // Control-thread integration point. Mapping owner must detach before unmapping.
    voice_studio::ipc::AudioBlockRealtimeAccess& audioRealtimeAccess() noexcept { return realtimeAccess; }
    void setAiConversionAvailable(bool available, std::uint64_t generation = 0) noexcept;
    bool isAiConversionAvailable() const noexcept { return aiConversionAvailable.load(std::memory_order_acquire); }
    bool launchWorker(const juce::File& executable);
    void stopWorker() noexcept;
    bool workerIsRunning() const noexcept;
    juce::String workerStatusText() const;

private:
    static APVTS::ParameterLayout createParameterLayout();
    std::array<juce::dsp::IIR::Filter<float>, 2> highPassFilters;
    std::array<juce::dsp::IIR::Filter<float>, 2> presenceFilters;
    std::array<juce::dsp::Compressor<float>, 2> compressors;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmoother;
    voice_studio::ipc::AudioBlockRealtimeAccess realtimeAccess;
#if defined(_WIN32)
    voice_studio::ipc::WindowsWorkerController workerController { realtimeAccess };
#endif
#if defined(_WIN32)
    void workerControlLoop() noexcept;
    void publishWorkerSnapshot() noexcept;
    std::mutex workerCommandMutex;
    std::condition_variable workerCommandWake;
    std::wstring pendingWorkerExecutable;
    bool workerLaunchPending = false;
    bool workerStopRequested = false;
    bool workerThreadExitRequested = false;
    std::thread workerControlThread;
    std::atomic<bool> workerThreadReady { false };
    std::atomic<bool> workerUiRunning { false };
    std::atomic<int> workerUiState { static_cast<int>(voice_studio::ipc::WorkerLifecycleState::disabled) };
#endif
    voice_studio::ipc::RvcAudioStreamAdapter rvcAudioStream;
#if defined(_WIN32)
    voice_studio::ipc::RvcAudioTransportPump rvcAudioPump { rvcAudioStream };
    std::array<float, voice_studio::ipc::RvcAudioStreamAdapter::kMaxHostCallbackFrames> rvcOutputScratch{};
#endif
    std::atomic<bool> aiConversionAvailable { false };
    std::atomic<std::uint64_t> aiGeneration { 0 };
    std::uint32_t preparedSampleRate = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceStudioAudioProcessor)
};
