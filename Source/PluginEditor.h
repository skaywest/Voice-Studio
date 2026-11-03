#pragma once
#include "PluginProcessor.h"

class VoiceStudioAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VoiceStudioAudioProcessorEditor(VoiceStudioAudioProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
private:
    void showPage(bool library);
    VoiceStudioAudioProcessor& processor;
    std::unique_ptr<juce::FileChooser> workerFileChooser;
    juce::TextButton effectsTab { "EFFECTS" }, libraryTab { "VOICE LIBRARY" };
    juce::ToggleButton aiButton { "AI VOICE (NOT IMPLEMENTED)" }, dspButton { "ENABLE DSP PROCESSING" };
    juce::ToggleButton highPassButton { "LOW CUT 80 Hz" }, presenceButton { "VOICE PRESENCE +2.5 dB" }, compressorButton { "COMPRESSOR 3:1" };
    juce::Slider outputGainSlider;
    juce::Label outputGainLabel, statusLabel, workerStatusLabel;
    juce::TextButton startWorkerButton { "SELECT WORKER EXE & START" }, stopWorkerButton { "STOP WORKER" };
    juce::Label libraryHeading, libraryDescription, categoryLabel, emptyState, safetyNote;
    juce::ComboBox categoryBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> aiAttachment, dspAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> highPassAttachment, presenceAttachment, compressorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputGainAttachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceStudioAudioProcessorEditor)
};
