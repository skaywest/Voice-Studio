cmake_minimum_required(VERSION 3.22)
project(VoiceStudio VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(VoiceStudio
    COMPANY_NAME "Whynot Audio"
    BUNDLE_ID "com.whynotaudio.voicestudio"
    PLUGIN_MANUFACTURER_CODE Whyn
    PLUGIN_CODE VcSt
    FORMATS VST3
    PRODUCT_NAME "Voice Studio"
    DESCRIPTION "Local voice processing host"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(VoiceStudio PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
juce_generate_juce_header(VoiceStudio)
target_compile_features(VoiceStudio PRIVATE cxx_std_17)
target_compile_definitions(VoiceStudio PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(VoiceStudio PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
