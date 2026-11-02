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
    target_include_directories(VoiceStudioProcessorSmokeTest PRIVATE Source)
    target_compile_features(VoiceStudioProcessorSmokeTest PRIVATE cxx_std_17)
    target_compile_definitions(VoiceStudioProcessorSmokeTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
    )
    # VoiceStudio keeps JUCE modules private; the standalone test compiles the
    # processor header directly, so it must receive those module include paths too.
    target_link_libraries(VoiceStudioProcessorSmokeTest PRIVATE
        VoiceStudio
        juce::juce_audio_utils
        juce::juce_dsp
    )
    add_test(NAME VoiceStudioProcessorSmokeTest COMMAND VoiceStudioProcessorSmokeTest)

    # This queue test is dependency-free and exercises the real-time handoff primitive.
    add_executable(VoiceStudioAudioQueueTest tests/AudioQueueTest.cpp)
    target_include_directories(VoiceStudioAudioQueueTest PRIVATE Source)
    target_compile_features(VoiceStudioAudioQueueTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioAudioQueueTest COMMAND VoiceStudioAudioQueueTest)

    # Windows-only, standalone cross-process shared-memory proof of concept.
    # This is a test executable; it is not loaded or launched by the VST3.
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_executable(VoiceStudioSharedMemoryPocTest tests/WindowsSharedMemoryPocTest.cpp)
        target_compile_features(VoiceStudioSharedMemoryPocTest PRIVATE cxx_std_17)
        target_link_libraries(VoiceStudioSharedMemoryPocTest PRIVATE bcrypt advapi32)
        add_test(NAME VoiceStudioSharedMemoryPocTest COMMAND VoiceStudioSharedMemoryPocTest)
        set_tests_properties(VoiceStudioSharedMemoryPocTest PROPERTIES TIMEOUT 90)
        add_test(NAME VoiceStudioSharedMemoryMalformedHeaderTest
                 COMMAND VoiceStudioSharedMemoryPocTest --malformed-header)
        add_test(NAME VoiceStudioSharedMemoryBadSlotSizeTest COMMAND VoiceStudioSharedMemoryPocTest --malformed-slot-size)
        add_test(NAME VoiceStudioSharedMemoryBadSampleCountTest COMMAND VoiceStudioSharedMemoryPocTest --malformed-sample-count)
        add_test(NAME VoiceStudioSharedMemoryBadReservedTest COMMAND VoiceStudioSharedMemoryPocTest --malformed-reserved)
        add_test(NAME VoiceStudioSharedMemoryBadMagicTest COMMAND VoiceStudioSharedMemoryPocTest --malformed-magic)
        add_test(NAME VoiceStudioSharedMemoryBadHeaderSizeTest COMMAND VoiceStudioSharedMemoryPocTest --malformed-header-size)
        add_test(NAME VoiceStudioSharedMemoryBadSlotCountTest COMMAND VoiceStudioSharedMemoryPocTest --malformed-slot-count)
        add_test(NAME VoiceStudioSharedMemoryNaNSampleTest COMMAND VoiceStudioSharedMemoryPocTest --corrupt-nan)
        add_test(NAME VoiceStudioSharedMemoryInfinitySampleTest COMMAND VoiceStudioSharedMemoryPocTest --corrupt-infinity)
        add_test(NAME VoiceStudioSharedMemoryWorkerExitTest
                 COMMAND VoiceStudioSharedMemoryPocTest --exit-after-ready)
        add_test(NAME VoiceStudioSharedMemoryWorkerCrashTest
                 COMMAND VoiceStudioSharedMemoryPocTest --terminate-after-ready)
        add_test(NAME VoiceStudioSharedMemoryWorkerCrashDuringTransferTest
                 COMMAND VoiceStudioSharedMemoryPocTest --terminate-during-transfer)
        add_test(NAME VoiceStudioSharedMemoryCorruptPayloadTest
                 COMMAND VoiceStudioSharedMemoryPocTest --corrupt-first-block)
        add_test(NAME VoiceStudioSharedMemoryRestartAfterCrashTest
                 COMMAND VoiceStudioSharedMemoryPocTest --restart-after-crash)
        set_tests_properties(
            VoiceStudioSharedMemoryPocTest
            VoiceStudioSharedMemoryMalformedHeaderTest
            VoiceStudioSharedMemoryBadSlotSizeTest
            VoiceStudioSharedMemoryBadSampleCountTest
            VoiceStudioSharedMemoryBadReservedTest
            VoiceStudioSharedMemoryBadMagicTest
            VoiceStudioSharedMemoryBadHeaderSizeTest
            VoiceStudioSharedMemoryBadSlotCountTest
            VoiceStudioSharedMemoryNaNSampleTest
            VoiceStudioSharedMemoryInfinitySampleTest
            VoiceStudioSharedMemoryWorkerExitTest
            VoiceStudioSharedMemoryWorkerCrashTest
            VoiceStudioSharedMemoryWorkerCrashDuringTransferTest
            VoiceStudioSharedMemoryCorruptPayloadTest
            VoiceStudioSharedMemoryRestartAfterCrashTest
            PROPERTIES TIMEOUT 90)
    endif()
endif()
