#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <utility>

namespace voice_studio::ipc
{
// Control-thread-only owner for the external Worker process. Never call from
// processBlock(): CreateProcess, waits, termination and handle management may
// block. The explicit application path prevents command-shell interpretation.
class WindowsWorkerProcess final
{
public:
    WindowsWorkerProcess() = default;
    WindowsWorkerProcess(const WindowsWorkerProcess&) = delete;
    WindowsWorkerProcess& operator=(const WindowsWorkerProcess&) = delete;

    ~WindowsWorkerProcess() { shutdown(); }

    bool launch(const std::wstring& executablePath,
                const std::wstring& arguments = {},
                const std::wstring& workingDirectory = {}) noexcept
    {
        if (process != nullptr || executablePath.empty())
            return false;

        HANDLE newJob = CreateJobObjectW(nullptr, nullptr);
        if (newJob == nullptr)
            return false;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(newJob, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)))
        {
            CloseHandle(newJob);
            return false;
        }

        // lpApplicationName is authoritative; quote argv[0] independently.
        std::wstring commandLine = L"\"" + executablePath + L"\"";
        if (!arguments.empty())
        {
            commandLine.push_back(L' ');
            commandLine += arguments;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        const DWORD flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;
        const BOOL created = CreateProcessW(
            executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            flags, nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup, &info);
        if (!created)
        {
            CloseHandle(newJob);
            return false;
        }

        CloseHandle(info.hThread);
        if (!AssignProcessToJobObject(newJob, info.hProcess))
        {
            TerminateProcess(info.hProcess, ERROR_ACCESS_DENIED);
            WaitForSingleObject(info.hProcess, 5000);
            CloseHandle(info.hProcess);
            CloseHandle(newJob);
            return false;
        }

        process = info.hProcess;
        job = newJob;
        processId = info.dwProcessId;
        return true;
    }

    bool isRunning() const noexcept
    {
        return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    // True only after Windows has signaled process death. Call this before
    // reclaiming any shared-memory slot owned by that Worker generation.
    bool hasExited() const noexcept
    {
        return process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    }

    bool waitForExit(DWORD timeoutMs) const noexcept
    {
        return process != nullptr && WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
    }

    bool terminate(UINT exitCode = ERROR_PROCESS_ABORTED) noexcept
    {
        if (process == nullptr)
            return false;
        if (hasExited())
            return true;
        if (!TerminateJobObject(job, exitCode))
            return false;
        return waitForExit(5000);
    }

    // Release handles only after exit is confirmed; closing the job also
    // guarantees any descendants in the job cannot outlive the Worker.
    bool closeAfterExit() noexcept
    {
        if (!hasExited())
            return false;
        closeHandles();
        return true;
    }

    void shutdown() noexcept
    {
        if (process == nullptr)
            return;
        if (!hasExited())
            terminate();
        if (hasExited())
            closeHandles();
    }

    DWORD id() const noexcept { return processId; }
    HANDLE nativeProcessHandle() const noexcept { return process; }

private:
    void closeHandles() noexcept
    {
        if (process != nullptr)
        {
            CloseHandle(process);
            process = nullptr;
        }
        if (job != nullptr)
        {
            CloseHandle(job);
            job = nullptr;
        }
        processId = 0;
    }

    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD processId = 0;
};
} // namespace voice_studio::ipc
