# Apply a narrowly-scoped JUCE 8.0.6 patch for the Voice Studio no-network experiment.
# This is intentionally pinned to exact source fragments: fail closed if upstream changes.
set(core_cpp "${juce_SOURCE_DIR}/modules/juce_core/juce_core.cpp")
set(win_cpp  "${juce_SOURCE_DIR}/modules/juce_core/native/juce_Network_windows.cpp")

file(READ "${core_cpp}" core_text)
foreach(fragment
    "#include \"network/juce_Socket.cpp\""
    "#include \"network/juce_URL.cpp\""
    "#include \"network/juce_WebInputStream.cpp\""
    "#include \"streams/juce_URLInputSource.cpp\"")
    string(FIND "${core_text}" "${fragment}" fragment_pos)
    if(fragment_pos EQUAL -1)
        message(FATAL_ERROR "JUCE networking patch: expected source fragment not found: ${fragment}")
    endif()
    string(REPLACE "${fragment}" "" core_text "${core_text}")
endforeach()
file(WRITE "${core_cpp}" "${core_text}")

file(READ "${win_cpp}" win_text)
set(pimpl_start "class WebInputStream::Pimpl")
set(pimpl_end "JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pimpl)\n};")
string(FIND "${win_text}" "${pimpl_start}" start_pos)
if(start_pos EQUAL -1)
    message(FATAL_ERROR "JUCE networking patch: WebInputStream Pimpl start not found")
endif()
string(FIND "${win_text}" "${pimpl_end}" end_pos)
if(end_pos EQUAL -1 OR end_pos LESS start_pos)
    message(FATAL_ERROR "JUCE networking patch: WebInputStream Pimpl end not found")
endif()
string(LENGTH "${pimpl_end}" end_len)
math(EXPR after_end "${end_pos} + ${end_len}")
string(SUBSTRING "${win_text}" 0 ${start_pos} before_pimpl)
math(EXPR pimpl_len "${after_end} - ${start_pos}")
string(SUBSTRING "${win_text}" ${start_pos} ${pimpl_len} pimpl_block)
string(SUBSTRING "${win_text}" ${after_end} -1 after_pimpl)
set(win_text "${before_pimpl}#if ! defined (JUCE_VOICE_STUDIO_DISABLE_NETWORKING)\n${pimpl_block}\n#endif\n${after_pimpl}")
file(WRITE "${win_cpp}" "${win_text}")

message(STATUS "Voice Studio: JUCE socket and HTTP/FTP implementations excluded for this experimental build")
