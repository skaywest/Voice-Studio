#include "AudioIpc/WindowsWorkerControlPipe.h"

#include <windows.h>

#include <string>

int main()
{
    using voice_studio::ipc::WindowsWorkerControlPipe;
    WindowsWorkerControlPipe pipe;
    if (pipe.createServer(L"") || pipe.isConnected())
        return 1;

    const std::wstring name = L"\\\\.\\pipe\\VoiceStudioControlTest-" +
                              std::to_wstring(GetCurrentProcessId());
    if (!pipe.createServer(name) || pipe.isConnected())
        return 2;

    // The server endpoint must be closable without a client and must not
    // claim a connection merely because CreateNamedPipe succeeded.
    pipe.close();
    if (pipe.isConnected())
        return 3;
    return 0;
}
