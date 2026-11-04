#include "AudioIpc/WindowsWorkerControlPipe.h"
#include "AudioIpc/WindowsWorkerProcess.h"
#include "AudioIpc/WorkerRuntimeManifestAbi.h"

#include <windows.h>

#include <string>

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

static void setModelPath(char16_t* destination, std::uint16_t& length, const char16_t* source)
{
    std::size_t count = 0;
    while (source[count] != 0) ++count;
    for (std::size_t i = 0; i <= count; ++i) destination[i] = source[i];
    length = static_cast<std::uint16_t>(count);
}

static WorkerModelConfigMessage makeModelConfig()
{
    WorkerModelConfigMessage message{};
    message.size = static_cast<std::uint16_t>(sizeof(message));
    message.generation = 17;
    message.requestId = 30;
    message.flags = kWorkerModelConfigFlagSourceTrusted |
                    kWorkerModelConfigFlagVoiceRightsConfirmed;
    setModelPath(message.rootPath, message.rootPathLength, u"C:\\RVC");
    setModelPath(message.entrypointPath, message.entrypointPathLength,
                 u"C:\\RVC\\RVCRealtimeVST\\worker\\rvc_worker.py");
    setModelPath(message.modelPath, message.modelPathLength, u"C:\\RVC\\Voices\\voice.pth");
    message.entrypointSizeBytes = 100;
    message.modelSizeBytes = 200;
    message.entrypointSha256[0] = 1;
    message.modelSha256[0] = 2;
    return message;
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

static int runPipeChild(const std::wstring& name, bool crashBeforeReply)
{
    WindowsWorkerControlPipe endpoint;
    if (!endpoint.connectClient(name, 5000))
        return 20;
    const auto command = makeCommand();
    if (!endpoint.sendCommand(command, 1000))
        return 21;
    if (crashBeforeReply)
        return 77;

    WorkerStatusMessage response{};
    if (!endpoint.receiveStatus(response, 3000))
        return 22;
    if (response.state != static_cast<std::uint16_t>(WorkerState::processing) ||
        response.generation != command.generation ||
        response.requestId != command.requestId ||
        response.heartbeatMonotonicNs != 123456789)
        return 23;
    return 0;
}

static bool runSeparateProcessExchange(const std::wstring& executable,
                                       const std::wstring& pipeName,
                                       bool crashBeforeReply)
{
    WindowsWorkerControlPipe server;
    if (!server.createServer(pipeName))
        return false;

    WindowsWorkerProcess worker;
    const std::wstring arguments = (crashBeforeReply ? L"--pipe-crash-child " : L"--pipe-child ") + pipeName;
    if (!worker.launch(executable, arguments))
        return false;

    if (!server.waitForClient(5000))
    {
        worker.terminate();
        return false;
    }

    WorkerCommandMessage command{};
    if (!server.receiveCommand(command, 3000) ||
        command.command != static_cast<std::uint16_t>(WorkerCommand::start) ||
        command.generation != 17 || command.requestId != 29)
    {
        worker.terminate();
        return false;
    }

    if (crashBeforeReply)
    {
        const bool exited = worker.waitForExit(5000);
        WorkerStatusMessage status{};
        const bool pipeClosed = !server.receiveStatus(status, 250);
        const bool result = exited && worker.hasExited() && pipeClosed;
        worker.closeAfterExit();
        return result;
    }

    if (!server.sendStatus(makeStatus(command), 3000) || !worker.waitForExit(5000))
    {
        worker.terminate();
        return false;
    }
    const bool cleanExit = worker.hasExited() && worker.closeAfterExit();
    server.close();
    return cleanExit;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring(argv[1]) == L"--pipe-child")
        return runPipeChild(argv[2], false);
    if (argc == 3 && std::wstring(argv[1]) == L"--pipe-crash-child")
        return runPipeChild(argv[2], true);

    WindowsWorkerControlPipe server;
    if (server.createServer(L"") || server.isConnected())
        return 1;

    const std::wstring name = L"\\\\.\\pipe\\VoiceStudioControlTest-" +
                              std::to_wstring(GetCurrentProcessId());
    if (!server.createServer(name) || server.isConnected())
        return 2;

    // In-process endpoint exchange still covers simultaneous client/server calls.
    bool inProcessOk = false;
    {
        WindowsWorkerControlPipe client;
        if (!client.connectClient(name, 5000) || !server.waitForClient(5000))
            return 3;
        const auto command = makeCommand();
        WorkerCommandMessage received{};
        if (!client.sendCommand(command) || !server.receiveCommand(received) ||
            received.generation != command.generation)
            return 4;
        if (!server.sendStatus(makeStatus(received)))
            return 5;
        WorkerStatusMessage response{};
        inProcessOk = client.receiveStatus(response) &&
                      response.requestId == command.requestId;
        if (!inProcessOk)
            return 6;

        const auto modelConfig = makeModelConfig();
        if (!hasValidWorkerModelConfig(modelConfig) ||
            !client.sendModelConfig(modelConfig, 3000))
            return 7;
        WorkerModelConfigMessage receivedConfig{};
        if (!server.receiveModelConfig(receivedConfig, 3000) ||
            receivedConfig.generation != modelConfig.generation ||
            receivedConfig.requestId != modelConfig.requestId ||
            receivedConfig.modelPathLength != modelConfig.modelPathLength ||
            !hasValidWorkerModelConfig(receivedConfig))
            return 8;


        WorkerRuntimeManifestHeader manifest{};
        manifest.size = static_cast<std::uint16_t>(sizeof(manifest));
        manifest.entryCount = 1;
        manifest.totalBytes = 12;
        manifest.manifestSha256[0] = 1; // Transport test only; digest checked by verifier tests.
        WorkerRuntimeManifestEntry manifestEntry{};
        manifestEntry.role = static_cast<std::uint16_t>(WorkerRuntimeFileRole::pythonSource);
        manifestEntry.pathLength = 14;
        const char16_t manifestPath[] = u"worker/main.py";
        for (std::size_t i = 0; i <= manifestEntry.pathLength; ++i)
            manifestEntry.relativePath[i] = manifestPath[i];
        manifestEntry.sizeBytes = 12;
        manifestEntry.sha256[0] = 1;
        if (!client.sendRuntimeManifest(manifest, &manifestEntry, 1, 3000))
            return 10;
        WorkerRuntimeManifestHeader receivedManifest{};
        std::vector<WorkerRuntimeManifestEntry> receivedEntries;
        if (!server.receiveRuntimeManifest(receivedManifest, receivedEntries, 3000) ||
            receivedManifest.entryCount != 1 || receivedEntries.size() != 1 ||
            receivedEntries[0].relativePath[0] != u'w')
            return 11;

        // Bounded chunk transport round-trip; staging and file writes are
        // intentionally separate from this transport-level test.
        WorkerRuntimeFileChunkHeader chunk{};
        chunk.entryIndex = 0;
        chunk.payloadBytes = 5;
        chunk.offsetBytes = 0;
        chunk.flags = kWorkerRuntimeFileChunkFlagFinal;
        chunk.size = static_cast<std::uint16_t>(sizeof(chunk));
        const std::uint8_t chunkPayload[5] = { 1, 2, 3, 4, 5 };
        if (!client.sendRuntimeFileChunk(chunk, chunkPayload, 3000))
            return 12;
        WorkerRuntimeFileChunkHeader receivedChunk{};
        std::uint8_t receivedPayload[kWorkerRuntimeFileChunkMaxBytes]{};
        if (!server.receiveRuntimeFileChunk(receivedChunk, receivedPayload,
                                             sizeof(receivedPayload), 3000) ||
            receivedChunk.entryIndex != 0 || receivedChunk.offsetBytes != 0 ||
            receivedChunk.payloadBytes != sizeof(chunkPayload) ||
            receivedChunk.flags != kWorkerRuntimeFileChunkFlagFinal ||
            receivedPayload[0] != 1 || receivedPayload[4] != 5)
            return 13;

        auto invalidChunk = chunk;
        invalidChunk.payloadBytes = kWorkerRuntimeFileChunkMaxBytes + 1;
        if (hasValidWorkerRuntimeFileChunkHeader(invalidChunk))
            return 14;
        invalidChunk = chunk;
        invalidChunk.flags = 2;
        if (hasValidWorkerRuntimeFileChunkHeader(invalidChunk))
            return 15;

        auto invalidConfig = modelConfig;
        invalidConfig.flags &= ~kWorkerModelConfigFlagVoiceRightsConfirmed;
        if (client.sendModelConfig(invalidConfig, 100))
            return 9;
    }
    server.close();
    if (!inProcessOk)
        return 6;

    // A larger-than-ABI command is not an idle poll. In message mode the
    // read reports ERROR_MORE_DATA; the endpoint must latch a protocol error
    // so the Worker cannot interpret the remainder as a later command.
    {
        const std::wstring malformedPipe = L"\\\\.\\pipe\\VoiceStudioMalformedCommand-" +
                                           std::to_wstring(GetCurrentProcessId());
        WindowsWorkerControlPipe malformedServer;
        if (!malformedServer.createServer(malformedPipe))
            return 16;
        HANDLE rawClient = CreateFileW(malformedPipe.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (rawClient == INVALID_HANDLE_VALUE)
            return 17;
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(rawClient, &mode, nullptr, nullptr) ||
            !malformedServer.waitForClient(3000))
        {
            CloseHandle(rawClient);
            return 18;
        }
        unsigned char oversized[sizeof(WorkerCommandMessage) + 8]{};
        DWORD written = 0;
        const bool sent = WriteFile(rawClient, oversized, sizeof(oversized), &written, nullptr) != FALSE &&
                          written == sizeof(oversized);
        WorkerCommandMessage ignored{};
        const bool accepted = malformedServer.receiveCommand(ignored, 1000);
        const bool rejectedAsProtocol = !accepted && malformedServer.hasProtocolError();
        CloseHandle(rawClient);
        malformedServer.close();
        if (!sent || !rejectedAsProtocol)
            return 19;
    }

    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return 7;

    const std::wstring suffix = std::to_wstring(GetCurrentProcessId());
    const std::wstring normalPipe = L"\\\\.\\pipe\\VoiceStudioControlProcess-" + suffix;
    const std::wstring crashPipe = L"\\\\.\\pipe\\VoiceStudioControlCrash-" + suffix;
    if (!runSeparateProcessExchange(executable, normalPipe, false))
        return 8;
    if (!runSeparateProcessExchange(executable, crashPipe, true))
        return 9;
    return 0;
}
