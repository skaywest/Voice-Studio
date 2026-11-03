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

    # Portable validation tests for the fixed shared-memory ABI layout.
    add_executable(VoiceStudioSharedMemoryAbiTest tests/SharedMemoryAbiTest.cpp)
    target_include_directories(VoiceStudioSharedMemoryAbiTest PRIVATE Source)
    target_compile_features(VoiceStudioSharedMemoryAbiTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioSharedMemoryAbiTest COMMAND VoiceStudioSharedMemoryAbiTest)

    # Production-sized, portable audio-block ABI layout and metadata validation tests.
    add_executable(VoiceStudioAudioBlockAbiTest tests/AudioBlockAbiTest.cpp)
    target_include_directories(VoiceStudioAudioBlockAbiTest PRIVATE Source)
    target_compile_features(VoiceStudioAudioBlockAbiTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioAudioBlockAbiTest COMMAND VoiceStudioAudioBlockAbiTest)

    # Versioned worker command/status wire-format validation.
    add_executable(VoiceStudioWorkerControlAbiTest tests/WorkerControlAbiTest.cpp)
    target_include_directories(VoiceStudioWorkerControlAbiTest PRIVATE Source)
    target_compile_features(VoiceStudioWorkerControlAbiTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioWorkerControlAbiTest COMMAND VoiceStudioWorkerControlAbiTest)

    # Control-thread worker lifecycle policy and fallback behavior.
    add_executable(VoiceStudioWorkerLifecycleTest tests/WorkerLifecycleTest.cpp)
    target_include_directories(VoiceStudioWorkerLifecycleTest PRIVATE Source)
    target_compile_features(VoiceStudioWorkerLifecycleTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioWorkerLifecycleTest COMMAND VoiceStudioWorkerLifecycleTest)

    # Control-thread facade and lock-free audio availability publication.
    add_executable(VoiceStudioWorkerControlCoordinatorTest tests/WorkerControlCoordinatorTest.cpp)
    target_include_directories(VoiceStudioWorkerControlCoordinatorTest PRIVATE Source)
    target_compile_features(VoiceStudioWorkerControlCoordinatorTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioWorkerControlCoordinatorTest COMMAND VoiceStudioWorkerControlCoordinatorTest)

    # Windows-only, standalone cross-process shared-memory proof of concept.
    # This is a test executable; it is not loaded or launched by the VST3.
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        # Standalone protocol Worker; no inference engine is linked yet.
        add_executable(VoiceStudioWorker Source/AudioIpc/WindowsWorkerMain.cpp)
        target_include_directories(VoiceStudioWorker PRIVATE Source)
        target_compile_features(VoiceStudioWorker PRIVATE cxx_std_17)
        target_link_libraries(VoiceStudioWorker PRIVATE advapi32)

        # Exercise the real controller against the separately launched Worker.
        add_executable(VoiceStudioWindowsWorkerControllerTest tests/WindowsWorkerControllerTest.cpp)
        target_include_directories(VoiceStudioWindowsWorkerControllerTest PRIVATE Source)
        target_compile_features(VoiceStudioWindowsWorkerControllerTest PRIVATE cxx_std_17)
        target_link_libraries(VoiceStudioWindowsWorkerControllerTest PRIVATE advapi32)
        add_test(NAME VoiceStudioWindowsWorkerControllerTest
                 COMMAND VoiceStudioWindowsWorkerControllerTest $<TARGET_FILE:VoiceStudioWorker>)
        set_tests_properties(VoiceStudioWindowsWorkerControllerTest PROPERTIES TIMEOUT 20)

        # Real separate-process audio payload exchange through the production Worker.
        add_executable(VoiceStudioWindowsAudioBlockWorkerProcessTest tests/WindowsAudioBlockWorkerProcessTest.cpp)
        target_include_directories(VoiceStudioWindowsAudioBlockWorkerProcessTest PRIVATE Source)
        target_compile_features(VoiceStudioWindowsAudioBlockWorkerProcessTest PRIVATE cxx_std_17)
        target_link_libraries(VoiceStudioWindowsAudioBlockWorkerProcessTest PRIVATE advapi32)
        add_test(NAME VoiceStudioWindowsAudioBlockWorkerProcessTest
                 COMMAND VoiceStudioWindowsAudioBlockWorkerProcessTest $<TARGET_FILE:VoiceStudioWorker>)
        set_tests_properties(VoiceStudioWindowsAudioBlockWorkerProcessTest PROPERTIES TIMEOUT 20)

        # Windows shared audio mapping owner/client and slot handoff test.
        add_executable(VoiceStudioWindowsAudioBlockMappingTest tests/WindowsAudioBlockMappingTest.cpp)
        target_include_directories(VoiceStudioWindowsAudioBlockMappingTest PRIVATE Source)
        target_compile_features(VoiceStudioWindowsAudioBlockMappingTest PRIVATE cxx_std_17)
        target_link_libraries(VoiceStudioWindowsAudioBlockMappingTest PRIVATE advapi32)
        add_test(NAME VoiceStudioWindowsAudioBlockMappingTest COMMAND VoiceStudioWindowsAudioBlockMappingTest)
        set_tests_properties(VoiceStudioWindowsAudioBlockMappingTest PROPERTIES TIMEOUT 20)

        # Control-thread-only Windows Worker process owner and lifecycle smoke test.
        add_executable(VoiceStudioWindowsWorkerProcessTest tests/WindowsWorkerProcessTest.cpp)
        target_include_directories(VoiceStudioWindowsWorkerProcessTest PRIVATE Source)
        target_compile_features(VoiceStudioWindowsWorkerProcessTest PRIVATE cxx_std_17)
        add_test(NAME VoiceStudioWindowsWorkerProcessTest COMMAND VoiceStudioWindowsWorkerProcessTest)
        set_tests_properties(VoiceStudioWindowsWorkerProcessTest PROPERTIES TIMEOUT 20)

        # Local-only, message-mode Worker control pipe endpoint smoke test.
        add_executable(VoiceStudioWindowsWorkerControlPipeTest tests/WindowsWorkerControlPipeTest.cpp)
        target_include_directories(VoiceStudioWindowsWorkerControlPipeTest PRIVATE Source)
        target_compile_features(VoiceStudioWindowsWorkerControlPipeTest PRIVATE cxx_std_17)
        target_link_libraries(VoiceStudioWindowsWorkerControlPipeTest PRIVATE advapi32)
        add_test(NAME VoiceStudioWindowsWorkerControlPipeTest COMMAND VoiceStudioWindowsWorkerControlPipeTest)
        set_tests_properties(VoiceStudioWindowsWorkerControlPipeTest PROPERTIES TIMEOUT 20)

        add_executable(VoiceStudioAudioBlockRealtimeAccessTest tests/AudioBlockRealtimeAccessTest.cpp)
        add_executable(VoiceStudioAudioBlockRealtimeBridgeTest tests/AudioBlockRealtimeBridgeTest.cpp)
        target_include_directories(VoiceStudioAudioBlockRealtimeBridgeTest PRIVATE Source)
        target_compile_features(VoiceStudioAudioBlockRealtimeBridgeTest PRIVATE cxx_std_17)
        add_test(NAME VoiceStudioAudioBlockRealtimeBridgeTest COMMAND VoiceStudioAudioBlockRealtimeBridgeTest)
        set_tests_properties(VoiceStudioAudioBlockRealtimeBridgeTest PROPERTIES TIMEOUT 20)
    target_include_directories(VoiceStudioAudioBlockRealtimeAccessTest PRIVATE Source)
    target_compile_features(VoiceStudioAudioBlockRealtimeAccessTest PRIVATE cxx_std_17)
    add_test(NAME VoiceStudioAudioBlockRealtimeAccessTest COMMAND VoiceStudioAudioBlockRealtimeAccessTest)

    add_executable(VoiceStudioAudioBlockSlotSyncTest tests/AudioBlockSlotSyncTest.cpp)
        target_include_directories(VoiceStudioAudioBlockSlotSyncTest PRIVATE Source)
        target_compile_features(VoiceStudioAudioBlockSlotSyncTest PRIVATE cxx_std_17)
        add_test(NAME VoiceStudioAudioBlockSlotSyncTest COMMAND VoiceStudioAudioBlockSlotSyncTest)

        # Exercise the AudioBlockSlot Interlocked ABI in a real second process.
        add_executable(VoiceStudioAudioBlockSlotProcessTest tests/WindowsAudioBlockSlotProcessTest.cpp)
        target_include_directories(VoiceStudioAudioBlockSlotProcessTest PRIVATE Source)
        target_compile_features(VoiceStudioAudioBlockSlotProcessTest PRIVATE cxx_std_17)
        add_test(NAME VoiceStudioAudioBlockSlotProcessTest COMMAND VoiceStudioAudioBlockSlotProcessTest)
        add_test(NAME VoiceStudioAudioBlockSlotWorkerCrashRecoveryTest
                 COMMAND VoiceStudioAudioBlockSlotProcessTest --worker-crash-recovery)
        set_tests_properties(
            VoiceStudioAudioBlockSlotProcessTest
            VoiceStudioAudioBlockSlotWorkerCrashRecoveryTest
            PROPERTIES TIMEOUT 45)

        add_executable(VoiceStudioSharedMemoryPocTest tests/WindowsSharedMemoryPocTest.cpp)
        target_include_directories(VoiceStudioSharedMemoryPocTest PRIVATE Source)
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
