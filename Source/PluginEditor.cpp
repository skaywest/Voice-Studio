#include "PluginEditor.h"

VoiceStudioAudioProcessorEditor::VoiceStudioAudioProcessorEditor(VoiceStudioAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(560, 340);
    aiButton.setEnabled(false); // AI worker is deliberately absent in this prototype.
    aiAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "aiEnabled", aiButton);
    dspAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "dspEnabled", dspButton);
    presetBox.addItemList({ "Radio Operator", "Deep Male", "Female Voice", "Robot", "Demon", "Anime Voice" }, 1);
    presetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "voicePreset", presetBox);
    statusLabel.setText("SAFE PROTOTYPE  |  AUDIO PASS-THROUGH  |  AI ENGINE NOT INSTALLED",
                        juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa9c7ae));
    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(aiButton);
    addAndMakeVisible(dspButton);
    addAndMakeVisible(presetBox);
    addAndMakeVisible(statusLabel);
}

void VoiceStudioAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff111820));
    g.setColour(juce::Colour(0xff1c2935));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 14.0f);
    g.setColour(juce::Colour(0xff75c7d5));
    g.setFont(juce::Font(28.0f, juce::Font::bold));
    g.drawText("VOICE STUDIO", 28, 22, 400, 42, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffb6c4cf));
    g.setFont(14.0f);
    g.drawText("LOCAL VOICE PROCESSING", 30, 62, 360, 22, juce::Justification::centredLeft);
}

void VoiceStudioAudioProcessorEditor::resized()
{
    aiButton.setBounds(32, 112, 490, 34);
    dspButton.setBounds(32, 158, 490, 34);
    presetBox.setBounds(32, 214, 490, 32);
    statusLabel.setBounds(24, 278, 512, 30);
}
