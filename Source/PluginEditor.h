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
    juce::ToggleButton dspButton { "DSP EFFECTS" };
    juce::ComboBox presetBox;
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> aiAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dspAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> presetAttachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceStudioAudioProcessorEditor)
};
