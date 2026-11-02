#include "PluginEditor.h"

VoiceStudioAudioProcessorEditor::VoiceStudioAudioProcessorEditor(VoiceStudioAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(560, 430);
    aiButton.setEnabled(false);
    aiAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters, "aiEnabled", aiButton);
    dspAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters, "dspEnabled", dspButton);
    highPassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters, "highPass", highPassButton);
    presenceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters, "presence", presenceButton);
    compressorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters, "compressor", compressorButton);

    outputGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    outputGainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 24);
    outputGainSlider.setTextValueSuffix(" dB");
    outputGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.parameters, "outputGain", outputGainSlider);
    outputGainLabel.setText("OUTPUT LEVEL", juce::dontSendNotification);
    outputGainLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb6c4cf));

    statusLabel.setText("DSP READY  |  AI ENGINE NOT INSTALLED", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa9c7ae));
    statusLabel.setJustificationType(juce::Justification::centred);

    for (auto* c : { static_cast<juce::Component*>(&aiButton), &dspButton, &highPassButton, &presenceButton, &compressorButton, &outputGainSlider, &outputGainLabel, &statusLabel })
        addAndMakeVisible(c);
}

void VoiceStudioAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff111820));
    g.setColour(juce::Colour(0xff1c2935));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 14.0f);
    g.setColour(juce::Colour(0xff75c7d5));
    g.setFont(juce::Font(28.0f, juce::Font::bold));
    g.drawText("VOICE STUDIO", 28, 18, 400, 42, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffb6c4cf));
    g.setFont(14.0f);
    g.drawText("LOCAL DSP VOICE PROCESSING", 30, 58, 360, 22, juce::Justification::centredLeft);
}

void VoiceStudioAudioProcessorEditor::resized()
{
    aiButton.setBounds(32, 96, 490, 30);
    dspButton.setBounds(32, 136, 490, 30);
    highPassButton.setBounds(48, 178, 460, 28);
    presenceButton.setBounds(48, 212, 460, 28);
    compressorButton.setBounds(48, 246, 460, 28);
    outputGainLabel.setBounds(32, 294, 150, 26);
    outputGainSlider.setBounds(170, 294, 350, 26);
    statusLabel.setBounds(24, 360, 512, 30);
}
