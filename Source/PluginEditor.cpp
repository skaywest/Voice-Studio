#include "PluginEditor.h"

VoiceStudioAudioProcessorEditor::VoiceStudioAudioProcessorEditor(VoiceStudioAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(580, 490);
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
    statusLabel.setText("DSP READY  |  AI INFERENCE NOT INSTALLED", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa9c7ae));
    statusLabel.setJustificationType(juce::Justification::centred);
    workerStatusLabel.setText(processor.workerStatusText(), juce::dontSendNotification);
    workerStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff75c7d5));
    workerStatusLabel.setJustificationType(juce::Justification::centred);
    startWorkerButton.onClick = [this]
    {
        if (workerFileChooser != nullptr)
            return;

        workerFileChooser = std::make_unique<juce::FileChooser>(
            "Select VoiceStudioWorker.exe", juce::File{}, "*.exe");
        juce::Component::SafePointer<VoiceStudioAudioProcessorEditor> safeThis(this);
        workerFileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& chooser)
            {
                if (safeThis == nullptr)
                    return;

                const auto selectedFile = chooser.getResult();
                if (selectedFile != juce::File{})
                {
                    if (!safeThis->processor.launchWorker(selectedFile))
                        safeThis->workerStatusLabel.setText(
                            "WORKER START FAILED - CHECK EXE / PERMISSIONS", juce::dontSendNotification);
                    else
                        safeThis->workerStatusLabel.setText(
                            safeThis->processor.workerStatusText(), juce::dontSendNotification);
                }

                safeThis->workerFileChooser.reset();
            });
    };
    stopWorkerButton.onClick = [this]
    {
        processor.stopWorker();
        workerStatusLabel.setText(processor.workerStatusText(), juce::dontSendNotification);
    };

    effectsTab.onClick = [this] { showPage(false); };
    libraryTab.onClick = [this] { showPage(true); };
    libraryHeading.setText("VOICE LIBRARY", juce::dontSendNotification);
    libraryHeading.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    libraryHeading.setColour(juce::Label::textColourId, juce::Colour(0xff75c7d5));
    libraryDescription.setText("Browse the local voice collection. No models are installed yet.", juce::dontSendNotification);
    libraryDescription.setColour(juce::Label::textColourId, juce::Colour(0xffb6c4cf));
    categoryLabel.setText("CATEGORY", juce::dontSendNotification);
    categoryLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb6c4cf));
    categoryBox.addItem("All voices", 1);
    categoryBox.addItem("Male", 2);
    categoryBox.addItem("Female", 3);
    categoryBox.addItem("Robot", 4);
    categoryBox.addItem("Demon", 5);
    categoryBox.addItem("Anime / stylized", 6);
    categoryBox.setSelectedId(1, juce::dontSendNotification);
    emptyState.setText("THE LIBRARY IS EMPTY", juce::dontSendNotification);
    emptyState.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    emptyState.setJustificationType(juce::Justification::centred);
    emptyState.setColour(juce::Label::textColourId, juce::Colour(0xffe3edf2));
    safetyNote.setText("Models will be listed only after source, licence, voice rights, SHA-256 and compatibility checks. Downloads are not available in this prototype.", juce::dontSendNotification);
    safetyNote.setColour(juce::Label::textColourId, juce::Colour(0xffa9c7ae));
    safetyNote.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(effectsTab); addAndMakeVisible(libraryTab);
    addAndMakeVisible(aiButton); addAndMakeVisible(dspButton);
    addAndMakeVisible(highPassButton); addAndMakeVisible(presenceButton); addAndMakeVisible(compressorButton);
    addAndMakeVisible(outputGainSlider); addAndMakeVisible(outputGainLabel); addAndMakeVisible(statusLabel);
    addAndMakeVisible(startWorkerButton); addAndMakeVisible(stopWorkerButton); addAndMakeVisible(workerStatusLabel);
    addAndMakeVisible(libraryHeading); addAndMakeVisible(libraryDescription);
    addAndMakeVisible(categoryLabel); addAndMakeVisible(categoryBox); addAndMakeVisible(emptyState); addAndMakeVisible(safetyNote);
    showPage(false);
    startTimerHz(4);
}

void VoiceStudioAudioProcessorEditor::showPage(bool library)
{
    aiButton.setVisible(!library); dspButton.setVisible(!library);
    startWorkerButton.setVisible(!library); stopWorkerButton.setVisible(!library);
    workerStatusLabel.setVisible(!library);
    highPassButton.setVisible(!library); presenceButton.setVisible(!library); compressorButton.setVisible(!library);
    outputGainSlider.setVisible(!library); outputGainLabel.setVisible(!library); statusLabel.setVisible(!library);
    libraryHeading.setVisible(library); libraryDescription.setVisible(library);
    categoryLabel.setVisible(library); categoryBox.setVisible(library);
    emptyState.setVisible(library); safetyNote.setVisible(library);
    effectsTab.setColour(juce::TextButton::buttonColourId, library ? juce::Colour(0xff263542) : juce::Colour(0xff31515d));
    libraryTab.setColour(juce::TextButton::buttonColourId, library ? juce::Colour(0xff31515d) : juce::Colour(0xff263542));
}

void VoiceStudioAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff111820));
    g.setColour(juce::Colour(0xff1c2935));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 14.0f);
    g.setColour(juce::Colour(0xff75c7d5));
    g.setFont(juce::Font(juce::FontOptions(27.0f, juce::Font::bold)));
    g.drawText("VOICE STUDIO", 28, 14, 400, 38, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffb6c4cf));
    g.setFont(13.0f);
    g.drawText("LOCAL VOICE PROCESSING", 30, 51, 360, 20, juce::Justification::centredLeft);
}

void VoiceStudioAudioProcessorEditor::resized()
{
    effectsTab.setBounds(30, 76, 245, 32); libraryTab.setBounds(280, 76, 270, 32);
    aiButton.setBounds(32, 128, 510, 30); dspButton.setBounds(32, 166, 510, 30);
    highPassButton.setBounds(48, 206, 480, 28); presenceButton.setBounds(48, 240, 480, 28);
    compressorButton.setBounds(48, 274, 480, 28); outputGainLabel.setBounds(32, 320, 150, 26);
    outputGainSlider.setBounds(170, 320, 370, 26);
    startWorkerButton.setBounds(32, 360, 330, 30); stopWorkerButton.setBounds(370, 360, 172, 30);
    workerStatusLabel.setBounds(24, 398, 532, 24); statusLabel.setBounds(24, 430, 532, 28);
    libraryHeading.setBounds(34, 132, 500, 30); libraryDescription.setBounds(34, 166, 510, 26);
    categoryLabel.setBounds(34, 210, 100, 26); categoryBox.setBounds(140, 210, 390, 28);
    emptyState.setBounds(40, 270, 500, 52); safetyNote.setBounds(42, 344, 496, 68);
}

void VoiceStudioAudioProcessorEditor::timerCallback()
{
    processor.serviceWorker();
    workerStatusLabel.setText(processor.workerStatusText(), juce::dontSendNotification);
    const bool running = processor.workerIsRunning();
    startWorkerButton.setEnabled(!running && workerFileChooser == nullptr);
    stopWorkerButton.setEnabled(running);
}
