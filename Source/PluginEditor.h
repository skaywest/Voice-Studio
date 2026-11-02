#pragma once
#include "PluginProcessor.h"

class VoiceStudioAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit VoiceStudioAudioProcessorEditor(VoiceStudioAudioProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    VoiceStudioAudioProcessor& processor;
    juce::ToggleButton aiButton { "AI VOICE (NOT IMPLEMENTED)" };
    juce::ToggleButton dspButton { "ENABLE DSP PROCESSING" };
    juce::ToggleButton highPassButton { "LOW CUT 80 Hz" };
    juce::ToggleButton presenceButton { "VOICE PRESENCE +2.5 dB" };
    juce::ToggleButton compressorButton { "COMPRESSOR 3:1" };
    juce::Slider outputGainSlider;
    juce::Label outputGainLabel;
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> aiAttachment, dspAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> highPassAttachment, presenceAttachment, compressorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputGainAttachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceStudioAudioProcessorEditor)
};
