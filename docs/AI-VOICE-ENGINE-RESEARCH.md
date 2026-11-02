# AI Voice Engine Research and Integration Plan

Status: research only. No engine, runtime, model, installer, or network capability is added by this document.

## Product target

Add optional, local RVC voice conversion to the existing Voice Studio VST3 for Windows x64 and Wave Link 3. The current DSP path must remain usable without an AI runtime. AI stays disabled by default. No cloud inference, telemetry, account sign-in, game-process access, injection, kernel driver, or security-software bypass is permitted.

## Findings from upstream projects

### RVC Realtime VST (RVC-Project)

The upstream developer guide describes a C++17 VST with model loading and inference in a separate Python worker. The audio thread handles buffers, mixing, and lock-free queues. It targets Windows 10/11 x64 and supports external RVC Python runtime, .pth models, optional .index files, and multiple F0 methods.

This is a useful architectural reference because it explicitly separates inference from the host audio callback. It does not ship a complete Python runtime or models, so packaging and dependency review remain necessary.

Source: https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI/tree/main/RVCRealtimeVST

### RVC Realtime (Rust / ONNX Runtime)

A newer Windows project ports the official real-time RVC path to Rust and ONNX Runtime with CUDA Graphs. Its published status is alpha. It reports RVC v2 F0 model support, while RVC v1, non-F0 models, index files, and non-NVIDIA devices are listed as unsupported. Its documented measurements are project-author measurements on an RTX 3060 Ti and are not a performance guarantee for Voice Studio.

This is a promising candidate for a later isolated engine prototype, but its alpha status, supported model subset, CUDA/cuDNN runtime requirements, licensing, and redistribution contents must be independently reviewed before adoption.

Source: https://github.com/Ramo-Inc/rvc-realtime

### vc-rs

This project documents Windows RVC conversion with a VST3 plugin and standalone GUI, using Windows ML or NVIDIA TensorRT paths. It may be useful as a reference for model conversion and deployment options. It is not adopted as a dependency.

Source: https://github.com/shirohata/vc-rs

## Provisional architecture

1. Keep the VST3 audio callback real-time safe: no disk access, model loading, process creation, network access, blocking locks, waits, or inference inside processBlock.
2. Run inference in a separate, visible, user-controlled local worker process.
3. Use bounded, versioned local IPC. Prefer shared memory with a narrowly scoped named pipe for control; do not use TCP/UDP sockets or loopback networking.
4. The audio callback writes fixed-size audio blocks to a preallocated lock-free input ring and reads the newest completed output block from a preallocated output ring.
5. If the worker is absent, starting, stopped, late, crashed, or produces invalid output, switch to a defined dry/DSP fallback without blocking the host.
6. Worker lifecycle is explicit: Start AI, Stop AI, status, and immediate stop. No auto-start at plugin load. No silent background downloads.
7. Models are data only. Accept only documented model formats; never execute a model, script, DLL, installer, or arbitrary plugin supplied as a voice model.
8. Model activation requires an explicit user action, source and license review, voice-rights review, file-size limits, SHA-256 verification, and format validation.
9. Runtime binaries and model assets must be version-pinned, reproducibly sourced, scanned, and included only after license and dependency review.
10. The DSP-only mode must remain fully functional if every AI component is removed.

## Required engineering gates before implementation

- Confirm Wave Link 3 VST3 host behavior and supported channel layouts on the target machine.
- Select a candidate engine only after reviewing its source, license, transitive dependencies, model format parser, and runtime DLLs.
- Demonstrate that the worker can run without outbound network access; enforce this at the OS/process boundary where practical rather than relying only on import-table scans.
- Measure end-to-end latency, audio underruns, CPU/GPU use, and recovery from worker termination on the target hardware.
- Add automated tests for ring-buffer overflow/underflow, stale blocks, worker crash, malformed IPC messages, invalid model files, and dry fallback.
- Do not install or modify anything on the user's PC until the user explicitly approves a reviewed test package and installation plan.

## Current decision

No AI engine is selected or integrated yet. The Rust/ONNX approach is a candidate for a contained prototype, not an approved production dependency. The Python-worker RVC VST architecture remains a useful reference for audio-thread isolation. The next implementation milestone is documented in [LOCAL-AI-WORKER-IPC-PROTOCOL.md](LOCAL-AI-WORKER-IPC-PROTOCOL.md). It is a design specification only; implementation should begin with protocol/queue tests and a reviewed shared-memory ABI, not downloading models or enabling AI in the existing plugin.
