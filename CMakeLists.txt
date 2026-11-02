cmake_minimum_required(VERSION 3.22)
project(VoiceStudio VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(JUCE)
if(NOT juce_POPULATED)
    FetchContent_Populate(JUCE)
    # JUCE bootstraps juceaide by reconfiguring its source tree. Keep the JUCE
    # sources pristine until that bootstrap has completed, then apply the
    # no-network patch for the Voice Studio module build only.
    add_subdirectory(${juce_SOURCE_DIR} ${juce_BINARY_DIR} EXCLUDE_FROM_ALL)
    include(cmake/DisableJuceNetworking.cmake)
    add_compile_definitions(JUCE_VOICE_STUDIO_DISABLE_NETWORKING=1)
endif()

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

option(VOICE_STUDIO_BUILD_SMOKE_TESTS "Build headless processor smoke tests" ON)
if(VOICE_STUDIO_BUILD_SMOKE_TESTS)
    enable_testing()
    add_executable(VoiceStudioProcessorSmokeTest tests/ProcessorSmokeTest.cpp)
    target_compile_features(VoiceStudioProcessorSmokeTest PRIVATE cxx_std_17)
    target_compile_definitions(VoiceStudioProcessorSmokeTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
    )
    target_link_libraries(VoiceStudioProcessorSmokeTest PRIVATE VoiceStudio)
    add_test(NAME VoiceStudioProcessorSmokeTest COMMAND VoiceStudioProcessorSmokeTest)
endif()
