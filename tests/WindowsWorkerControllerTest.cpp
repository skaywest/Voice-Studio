#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include "AudioIpc/WindowsWorkerController.h"
#include <cstdint>
#include <string>
#include <cwchar>

using namespace voice_studio::ipc;

static std::uint64_t monotonicNs() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64()) * 1'000'000ull;
}

static bool writeFile(const std::wstring& path) noexcept
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr char payload[] = "abc";
    DWORD written = 0;
    const bool ok = WriteFile(file, payload, 3, &written, nullptr) != FALSE && written == 3;
    CloseHandle(file);
    return ok;
}

static void setPath(char16_t* dst, std::uint16_t& length, const std::wstring& src)
{
    length = static_cast<std::uint16_t>(src.size());
    for (std::size_t i = 0; i <= src.size(); ++i)
        dst[i] = static_cast<char16_t>(src[i]);
}

static bool createManifest(WorkerModelConfigMessage& config, std::wstring& root,
                           std::wstring& entrypoint, std::wstring& model)
{
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) return false;
    root = std::wstring(temp) + L"VoiceStudioModelConfig-" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(root.c_str(), nullptr)) return false;
    const auto rvc = root + L"\\RVCRealtimeVST";
    const auto worker = rvc + L"\\worker";
    const auto models = root + L"\\models";
    if (!CreateDirectoryW(rvc.c_str(), nullptr) ||
        !CreateDirectoryW(worker.c_str(), nullptr) ||
        !CreateDirectoryW(models.c_str(), nullptr)) return false;
    entrypoint = worker + L"\\rvc_worker.py";
    model = models + L"\\voice.pth";
    if (!writeFile(entrypoint) || !writeFile(model)) return false;

    config = WorkerModelConfigMessage{};
    config.size = static_cast<std::uint16_t>(sizeof(config));
    config.flags = kWorkerModelConfigFlagSourceTrusted | kWorkerModelConfigFlagVoiceRightsConfirmed;
    config.entrypointSizeBytes = 3;
    config.modelSizeBytes = 3;
    constexpr std::uint8_t abcSha256[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    for (std::size_t i = 0; i < 32; ++i)
    {
        config.entrypointSha256[i] = abcSha256[i];
        config.modelSha256[i] = abcSha256[i];
    }
    setPath(config.rootPath, config.rootPathLength, root);
    setPath(config.entrypointPath, config.entrypointPathLength, entrypoint);
    setPath(config.modelPath, config.modelPathLength, model);
    return hasValidWorkerModelConfig(config);
}

static void cleanup(const std::wstring& root, const std::wstring& entrypoint,
                    const std::wstring& model) noexcept
{
    if (!entrypoint.empty()) DeleteFileW(entrypoint.c_str());
    if (!model.empty()) DeleteFileW(model.c_str());
    if (!root.empty())
    {
        RemoveDirectoryW((root + L"\\RVCRealtimeVST\\worker").c_str());
        RemoveDirectoryW((root + L"\\RVCRealtimeVST").c_str());
        RemoveDirectoryW((root + L"\\models").c_str());
        RemoveDirectoryW(root.c_str());
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    const std::wstring executable = argv[1];
    const std::wstring pipeName = L"\\\\.\\pipe\\VoiceStudioControllerTest-" +
                                  std::to_wstring(GetCurrentProcessId());
    WorkerModelConfigMessage config{};
    std::wstring root, entrypoint, model;
    if (!createManifest(config, root, entrypoint, model))
    {
        cleanup(root, entrypoint, model);
        return 10;
    }

    WindowsWorkerController controller;
    if (!controller.launch(executable, pipeName, monotonicNs(), 5000))
    {
        cleanup(root, entrypoint, model);
        return 2;
    }
    if (controller.currentState() != WorkerLifecycleState::ready ||
        controller.audioConversionAvailable() ||
        !controller.audioRealtimeAccess().isAttached())
    {
        controller.shutdown(); cleanup(root, entrypoint, model); return 3;
    }

    // This traverses the real message-mode pipe and verifies both files in
    // the separate Worker process; success must not mark AI as running.
    if (!controller.configureModel(config, 5000) ||
        controller.currentState() != WorkerLifecycleState::ready ||
        controller.audioConversionAvailable())
    {
        controller.shutdown(); cleanup(root, entrypoint, model); return 4;
    }

    if (!controller.enableAi(3000))
    {
        controller.shutdown(); cleanup(root, entrypoint, model); return 5;
    }

    bool receivedFailure = false;
    for (int i = 0; i < 20; ++i)
    {
        if (!controller.pollStatus(250))
        {
            receivedFailure = controller.currentState() == WorkerLifecycleState::faulted;
            if (receivedFailure) break;
        }
    }
    const bool safeFallback = receivedFailure && !controller.audioConversionAvailable() &&
                              !controller.audioRealtimeAccess().isAttached();
    controller.shutdown();
    if (!safeFallback)
    {
        cleanup(root, entrypoint, model);
        return 6;
    }

    // A syntactically valid manifest with a wrong digest must be rejected by
    // the Worker itself, and the controller must fail closed.
    auto tampered = config;
    tampered.modelSha256[0] ^= 0xffu;
    const std::wstring badPipeName = pipeName + L"-bad-hash";
    WindowsWorkerController badController;
    if (!badController.launch(executable, badPipeName, monotonicNs(), 5000))
    {
        cleanup(root, entrypoint, model);
        return 7;
    }
    const bool rejected = !badController.configureModel(tampered, 5000) &&
                          badController.currentState() == WorkerLifecycleState::faulted &&
                          !badController.audioConversionAvailable();
    badController.shutdown();
    cleanup(root, entrypoint, model);
    return rejected ? 0 : 8;
}
