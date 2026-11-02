#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "WorkerControlAbi.h"

#include <cstdint>
#include <string>

namespace voice_studio::ipc
{
// Synchronous message-mode Named Pipe endpoint. Every method is control-thread
// only: never call from an audio callback. The caller supplies an unpredictable
// per-session pipe name and starts the Worker with that name out-of-band.
class WindowsWorkerControlPipe final
{
public:
    WindowsWorkerControlPipe() = default;
    WindowsWorkerControlPipe(const WindowsWorkerControlPipe&) = delete;
    WindowsWorkerControlPipe& operator=(const WindowsWorkerControlPipe&) = delete;
    ~WindowsWorkerControlPipe() { close(); }

    bool createServer(const std::wstring& pipeName) noexcept
    {
        if (pipe != INVALID_HANDLE_VALUE || pipeName.empty())
            return false;

        HANDLE handle = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1,
            static_cast<DWORD>(sizeof(WorkerStatusMessage)),
            static_cast<DWORD>(sizeof(WorkerCommandMessage)),
            0,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        pipe = handle;
        connected = false;
        serverEndpoint = true;
        return true;
    }

    bool connectClient(const std::wstring& pipeName, DWORD timeoutMs = 3000) noexcept
    {
        if (pipe != INVALID_HANDLE_VALUE || pipeName.empty())
            return false;
        if (!WaitNamedPipeW(pipeName.c_str(), timeoutMs))
            return false;
        HANDLE handle = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr))
        {
            CloseHandle(handle);
            return false;
        }
        pipe = handle;
        connected = true;
        serverEndpoint = false;
        return true;
    }

    bool waitForClient() noexcept
    {
        if (pipe == INVALID_HANDLE_VALUE || connected)
            return false;
        const BOOL result = ConnectNamedPipe(pipe, nullptr);
        if (result)
        {
            connected = true;
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            connected = true;
            return true;
        }
        return false;
    }

    bool sendCommand(const WorkerCommandMessage& message) noexcept
    {
        return connected && hasValidWorkerCommand(message) &&
               writeMessage(&message, static_cast<DWORD>(sizeof(message)));
    }

    bool receiveStatus(WorkerStatusMessage& output) noexcept
    {
        WorkerStatusMessage message{};
        if (!connected || !readExactMessage(&message, static_cast<DWORD>(sizeof(message))) ||
            !hasValidWorkerStatus(message))
            return false;
        output = message;
        return true;
    }

    bool sendStatus(const WorkerStatusMessage& message) noexcept
    {
        return connected && hasValidWorkerStatus(message) &&
               writeMessage(&message, static_cast<DWORD>(sizeof(message)));
    }

    bool receiveCommand(WorkerCommandMessage& output) noexcept
    {
        WorkerCommandMessage message{};
        if (!connected || !readExactMessage(&message, static_cast<DWORD>(sizeof(message))) ||
            !hasValidWorkerCommand(message))
            return false;
        output = message;
        return true;
    }

    void disconnect() noexcept
    {
        if (pipe != INVALID_HANDLE_VALUE)
        {
            // Do not FlushFileBuffers here: it can block until the peer reads
            // every buffered byte. Disconnect must remain bounded by the
            // control thread's own policy, not by an unresponsive Worker.
            if (connected && serverEndpoint)
                DisconnectNamedPipe(pipe);
        }
        connected = false;
    }

    void close() noexcept
    {
        if (pipe != INVALID_HANDLE_VALUE)
        {
            disconnect();
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }

    bool isConnected() const noexcept { return connected; }

private:
    bool writeMessage(const void* data, DWORD size) noexcept
    {
        DWORD written = 0;
        return WriteFile(pipe, data, size, &written, nullptr) != FALSE &&
               written == size;
    }

    bool readExactMessage(void* data, DWORD expectedSize) noexcept
    {
        DWORD received = 0;
        const BOOL ok = ReadFile(pipe, data, expectedSize, &received, nullptr);
        // Message-mode pipes report ERROR_MORE_DATA when a peer sends a
        // larger frame. Reject it rather than accepting a truncated ABI.
        return ok != FALSE && received == expectedSize;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool connected = false;
    bool serverEndpoint = false;
};
} // namespace voice_studio::ipc
