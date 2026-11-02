# Disable JUCE network functionality while preserving local URL/file APIs.
# Pinned to JUCE 8.0.6 source fragments; fail closed if upstream changes.
set(core_cpp "${juce_SOURCE_DIR}/modules/juce_core/juce_core.cpp")
set(url_cpp "${juce_SOURCE_DIR}/modules/juce_core/network/juce_URL.cpp")
set(events_cpp "${juce_SOURCE_DIR}/modules/juce_events/juce_events.cpp")
set(win_cpp "${juce_SOURCE_DIR}/modules/juce_core/native/juce_Network_windows.cpp")

# Remove socket and WebInputStream implementations, but retain URL.cpp and
# URLInputSource.cpp so local file URLs and GUI URL APIs remain available.
file(READ "${core_cpp}" core_text)
foreach(fragment
    "#include \"network/juce_Socket.cpp\""
    "#include \"network/juce_WebInputStream.cpp\"")
    string(FIND "${core_text}" "${fragment}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "JUCE patch: expected core include missing: ${fragment}")
    endif()
    string(REPLACE "${fragment}" "" core_text "${core_text}")
endforeach()
file(WRITE "${core_cpp}" "${core_text}")

# These JUCE event helpers are network services and are not used by this plugin.
file(READ "${events_cpp}" events_text)
foreach(fragment
    "#include \"interprocess/juce_InterprocessConnection.cpp\""
    "#include \"interprocess/juce_InterprocessConnectionServer.cpp\""
    "#include \"interprocess/juce_NetworkServiceDiscovery.cpp\"")
    string(FIND "${events_text}" "${fragment}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "JUCE patch: expected events include missing: ${fragment}")
    endif()
    string(REPLACE "${fragment}" "" events_text "${events_text}")
endforeach()
file(WRITE "${events_cpp}" "${events_text}")

# Keep the URL value type and local-file helpers, but make every HTTP(S)
# stream request fail closed and disable JUCE's fallback HTTP downloader.
file(READ "${url_cpp}" url_text)
set(fallback_start "struct FallbackDownloadTask final")
set(fallback_end "void URL::DownloadTaskListener::progress")
string(FIND "${url_text}" "${fallback_start}" fallback_pos)
string(FIND "${url_text}" "${fallback_end}" listener_pos)
if(fallback_pos EQUAL -1 OR listener_pos LESS fallback_pos)
    message(FATAL_ERROR "JUCE patch: fallback downloader block not found")
endif()
string(SUBSTRING "${url_text}" 0 ${fallback_pos} url_before_fallback)
string(SUBSTRING "${url_text}" ${listener_pos} -1 url_after_fallback)
set(url_text "${url_before_fallback}${url_after_fallback}")

set(download_start "std::unique_ptr<URL::DownloadTask> URL::DownloadTask::createFallbackDownloader")
set(download_end "URL::DownloadTask::DownloadTask()")
string(FIND "${url_text}" "${download_start}" download_pos)
string(FIND "${url_text}" "${download_end}" download_end_pos)
if(download_pos EQUAL -1 OR download_end_pos LESS download_pos)
    message(FATAL_ERROR "JUCE patch: fallback downloader function not found")
endif()
set(download_stub "std::unique_ptr<URL::DownloadTask> URL::DownloadTask::createFallbackDownloader (const URL&, const File&, const DownloadTaskOptions&)\n{\n    return nullptr;\n}\n\n")
string(SUBSTRING "${url_text}" 0 ${download_pos} url_before_download)
string(SUBSTRING "${url_text}" ${download_end_pos} -1 url_after_download)
set(url_text "${url_before_download}${download_stub}${url_after_download}")

set(stream_start "std::unique_ptr<InputStream> URL::createInputStream (const InputStreamOptions& options) const")
set(stream_end "std::unique_ptr<OutputStream> URL::createOutputStream() const")
string(FIND "${url_text}" "${stream_start}" stream_pos)
string(FIND "${url_text}" "${stream_end}" stream_end_pos)
if(stream_pos EQUAL -1 OR stream_end_pos LESS stream_pos)
    message(FATAL_ERROR "JUCE patch: URL input stream function not found")
endif()
set(stream_stub "std::unique_ptr<InputStream> URL::createInputStream (const InputStreamOptions&) const\n{\n    if (isLocalFile())\n        return getLocalFile().createInputStream();\n\n    return nullptr;\n}\n\n")
string(SUBSTRING "${url_text}" 0 ${stream_pos} url_before_stream)
string(SUBSTRING "${url_text}" ${stream_end_pos} -1 url_after_stream)
set(url_text "${url_before_stream}${stream_stub}${url_after_stream}")
file(WRITE "${url_cpp}" "${url_text}")

# Remove the WinINet WebInputStream implementation from the Windows build.
file(READ "${win_cpp}" win_text)
set(pimpl_start "class WebInputStream::Pimpl")
set(pimpl_end "JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pimpl)\n};")
string(FIND "${win_text}" "${pimpl_start}" start_pos)
string(FIND "${win_text}" "${pimpl_end}" end_pos)
if(start_pos EQUAL -1 OR end_pos LESS start_pos)
    message(FATAL_ERROR "JUCE patch: Windows WebInputStream Pimpl not found")
endif()
string(LENGTH "${pimpl_end}" end_len)
math(EXPR after_end "${end_pos} + ${end_len}")
string(SUBSTRING "${win_text}" 0 ${start_pos} before_pimpl)
string(SUBSTRING "${win_text}" ${after_end} -1 after_pimpl)
set(win_text "${before_pimpl}${after_pimpl}")
file(WRITE "${win_cpp}" "${win_text}")

message(STATUS "Voice Studio: JUCE network APIs disabled; local URL/file support retained")
