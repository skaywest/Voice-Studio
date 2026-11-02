#include "AudioIpc/WindowsWorkerControlPipe.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

using namespace voice_studio::ipc;

static WorkerCommandMessage makeCommand()
{
    WorkerCommandMessage m{};
    m.magic = kWorkerControlMagic;
    m.version = kWorkerControlVersion;
    m.size = sizeof(m);
    m.command = static_cast<std::uint16_t>(WorkerCommand::start);
    m.generation = 17;
    m.requestId = 29;
    m.timeoutMs = 5000;
    return m;
}

static WorkerStatusMessage makeStatus(const WorkerCommandMessage& command)
{
    WorkerStatusMessage m{};
    m.magic = kWorkerControlMagic;
    m.version = kWorkerControlVersion;
    m.size = sizeof(m);
    m.state = static_cast<std::uint16_t>(WorkerState::processing);
    m.result = static_cast<std::uint32_t>(WorkerResult::ok);
    m.generation = command.generation;
    m.requestId = command.requestId;
    m.heartbeatMonotonicNs = 123456789;
    return m;
}

int main()
{
    WindowsWorkerControlPipe server;
    if (server.createServer(L"") || server.isConnected())
        return 1;

    const std::wstring name = L"\\\\.\\pipe\\VoiceStudioControlTest-" +
                              std::to_wstring(GetCurrentProcessId());
    if (!server.createServer(name) || server.isConnected())
        return 2;

    std::atomic<bool> clientOk{false};
    std::atomic<bool> keepClientOpen{true};
    std::thread client([&]
    {
        WindowsWorkerControlPipe endpoint;
        if (!endpoint.connectClient(name, 5000))
            return;
        const auto command = makeCommand();
        if (!endpoint.sendCommand(command))
            return;
        WorkerStatusMessage response{};
        if (!endpoint.receiveStatus(response))
            return;
        clientOk.store(response.state == static_cast<std::uint16_t>(WorkerState::processing) &&
                        response.generation == command.generation &&
                        response.requestId == command.requestId &&
                        response.heartbeatMonotonicNs == 123456789,
                        std::memory_order_release);
    });

    if (!server.waitForClient())
    {
        client.join();
        return 3;
    }
    WorkerCommandMessage received{};
    if (!server.receiveCommand(received))
    {
        server.close();
        client.join();
        return 4;
    }
    if (received.command != static_cast<std::uint16_t>(WorkerCommand::start) ||
        received.generation != 17 || received.requestId != 29)
    {
        server.close();
        client.join();
        return 5;
    }
    const auto status = makeStatus(received);
    if (!server.sendStatus(status))
    {
        server.close();
        client.join();
        return 6;
    }

    client.join();
    server.close();
    if (!clientOk.load(std::memory_order_acquire) || server.isConnected())
        return 7;
    return 0;
}
