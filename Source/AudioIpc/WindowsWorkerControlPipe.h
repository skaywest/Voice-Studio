#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include "WorkerControlAbi.h"
#include "WorkerModelConfigAbi.h"

#include <cstdint>
#include <string>

namespace voice_studio::ipc
{
// Message-mode Named Pipe endpoint. All calls are control-thread only;
// each pending operation is overlapped and bounded by its timeout.
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

        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &securityDescriptor, nullptr))
            return false;
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = securityDescriptor;
        securityAttributes.bInheritHandle = FALSE;

        HANDLE handle = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1,
            16384,
            16384,
            0,
            &securityAttributes);
        LocalFree(securityDescriptor);
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
                                    0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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

    bool waitForClient(DWORD timeoutMs = 3000) noexcept
    {
        if (pipe == INVALID_HANDLE_VALUE || connected)
            return false;

        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr)
            return false;

        const BOOL result = ConnectNamedPipe(pipe, &operation);
        if (result)
        {
            CloseHandle(operation.hEvent);
            connected = true;
            return true;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            CloseHandle(operation.hEvent);
            connected = true;
            return true;
        }
        if (error != ERROR_IO_PENDING)
        {
            CloseHandle(operation.hEvent);
            return false;
        }

        const DWORD wait = WaitForSingleObject(operation.hEvent, timeoutMs);
        if (wait != WAIT_OBJECT_0)
        {
            // The client may connect at the same instant as our timeout.
            // If cancellation loses that race, preserve the completed result.
            DWORD ignored = 0;
            connected = cancelAndDrain(operation, ignored);
            CloseHandle(operation.hEvent);
            return connected;
        }

        DWORD ignored = 0;
        const BOOL completed = GetOverlappedResult(pipe, &operation, &ignored, FALSE);
        CloseHandle(operation.hEvent);
        connected = completed != FALSE;
        return connected;
    }

    bool sendCommand(const WorkerCommandMessage& message, DWORD timeoutMs = 3000) noexcept
    {
        return connected && hasValidWorkerCommand(message) &&
               writeMessage(&message, static_cast<DWORD>(sizeof(message)), timeoutMs);
    }

    // Model configuration is a bounded, fixed-size message. It is sent only
    // from the control thread; the real-time audio callback never touches IPC.
    bool sendModelConfig(const WorkerModelConfigMessage& message, DWORD timeoutMs = 3000) noexcept
    {
        return connected && hasValidWorkerModelConfig(message) &&
               writeMessage(&message, static_cast<DWORD>(sizeof(message)), timeoutMs);
    }

    bool receiveModelConfig(WorkerModelConfigMessage& output, DWORD timeoutMs = 3000) noexcept
    {
        WorkerModelConfigMessage message{};
        if (!connected || !readExactMessage(&message, static_cast<DWORD>(sizeof(message)), timeoutMs) ||
            !hasValidWorkerModelConfig(message))
            return false;
        output = message;
        return true;
    }

    bool receiveStatus(WorkerStatusMessage& output, DWORD timeoutMs = 3000) noexcept
    {
        WorkerStatusMessage message{};
        if (!connected || !readExactMessage(&message, static_cast<DWORD>(sizeof(message)), timeoutMs) ||
            !hasValidWorkerStatus(message))
            return false;
        output = message;
        return true;
    }

    bool sendStatus(const WorkerStatusMessage& message, DWORD timeoutMs = 3000) noexcept
    {
        return connected && hasValidWorkerStatus(message) &&
               writeMessage(&message, static_cast<DWORD>(sizeof(message)), timeoutMs);
    }

    bool receiveCommand(WorkerCommandMessage& output, DWORD timeoutMs = 3000) noexcept
    {
        WorkerCommandMessage message{};
        if (!connected || !readExactMessage(&message, static_cast<DWORD>(sizeof(message)), timeoutMs) ||
            !hasValidWorkerCommand(message))
            return false;
        output = message;
        return true;
    }

    void disconnect() noexcept
    {
        if (pipe != INVALID_HANDLE_VALUE && connected && serverEndpoint)
            DisconnectNamedPipe(pipe);
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
    bool cancelAndDrain(OVERLAPPED& operation, DWORD& transferred) noexcept
    {
        // The OVERLAPPED storage and event must remain alive until the kernel
        // has completed cancellation; otherwise late completion can corrupt memory.
        // ERROR_NOT_FOUND means the operation may already have completed. In that
        // race, GetOverlappedResult tells us whether useful data was actually read.
        if (!CancelIoEx(pipe, &operation) && GetLastError() != ERROR_NOT_FOUND)
        {
            // Still wait below: cancellation failure does not make the request safe to forget.
        }
        WaitForSingleObject(operation.hEvent, INFINITE);
        DWORD completedBytes = 0;
        const BOOL completed = GetOverlappedResult(pipe, &operation, &completedBytes, FALSE);
        transferred = completedBytes;
        return completed != FALSE;
    }

    bool transfer(bool writing, const void* source, void* destination,
                  DWORD size, DWORD timeoutMs, DWORD& transferred) noexcept
    {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr)
            return false;

        BOOL completed = writing
            ? WriteFile(pipe, source, size, &transferred, &operation)
            : ReadFile(pipe, destination, size, &transferred, &operation);

        if (!completed)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                CloseHandle(operation.hEvent);
                return false;
            }

            const DWORD wait = WaitForSingleObject(operation.hEvent, timeoutMs);
            if (wait != WAIT_OBJECT_0)
            {
                // A read/write can complete just as the timeout fires. Do not
                // discard a successful completion merely because CancelIoEx
                // raced with it; this is especially important for short Worker
                // command-poll intervals on the receiving side.
                const bool completedDuringTimeout = cancelAndDrain(operation, transferred);
                CloseHandle(operation.hEvent);
                return completedDuringTimeout;
            }
            completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
        }

        CloseHandle(operation.hEvent);
        return completed != FALSE;
    }

    bool writeMessage(const void* data, DWORD size, DWORD timeoutMs) noexcept
    {
        DWORD written = 0;
        return transfer(true, data, nullptr, size, timeoutMs, written) && written == size;
    }

    bool readExactMessage(void* data, DWORD expectedSize, DWORD timeoutMs) noexcept
    {
        DWORD received = 0;
        return transfer(false, nullptr, data, expectedSize, timeoutMs, received) &&
               received == expectedSize;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool connected = false;
    bool serverEndpoint = false;
};
} // namespace voice_studio::ipc
