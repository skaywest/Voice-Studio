#include "AudioIpc/WindowsWorkerProcess.h"

#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"--worker-child")
    {
        for (;;)
            Sleep(1000);
    }

    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return 1;

    voice_studio::ipc::WindowsWorkerProcess worker;
    if (!worker.launch(executable, L"--worker-child"))
    {
        std::cerr << "Worker process launch failed\n";
        return 2;
    }
    if (worker.id() == 0 || !worker.isRunning())
    {
        std::cerr << "Worker did not enter running state\n";
        return 3;
    }
    if (worker.closeAfterExit())
    {
        std::cerr << "Live process was incorrectly accepted as exited\n";
        return 4;
    }
    if (!worker.terminate())
    {
        std::cerr << "Worker termination did not complete\n";
        return 5;
    }
    if (!worker.hasExited() || !worker.closeAfterExit() ||
        worker.nativeProcessHandle() != nullptr || worker.id() != 0)
    {
        std::cerr << "Exited Worker handles were not safely released\n";
        return 6;
    }

    std::cout << "Windows Worker process launch/monitor/terminate passed\n";
    return 0;
}
