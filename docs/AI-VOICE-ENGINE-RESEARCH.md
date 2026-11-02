# AI Voice Engine Research and Integration Plan

Status: candidate selected for a contained feasibility prototype; no engine, runtime, model, installer, or network capability is integrated.

Research checked: 2026-11-02.

## Product target

Add optional, local AI voice conversion to Voice Studio VST3 for Windows x64 and Wave Link 3. The current DSP path must remain usable without an AI runtime. AI stays disabled by default. No cloud inference, telemetry, account sign-in, game-process access, injection, kernel driver, or security-software bypass is permitted.

## Candidate assessment

### 1. RVC inference — preferred first prototype

RVC is a genuine voice-timbre conversion model family, not a DSP pitch/equalizer effect. The upstream project is MIT licensed. Its current Windows instructions target Python 3.12 x64 and explicitly separate NVIDIA RTX 50-series dependencies from older NVIDIA GPUs: RTX 50-series uses PyTorch 2.7.1 + CUDA 12.8 (cu128), while pre-RTX-50 NVIDIA uses the cu118 package set.

The upstream RVC Realtime VST is especially relevant as an architecture reference: C++17 VST2/VST3, a separate Python worker, lock-free audio queues, shared memory and Windows Events. Its code in the RVCRealtimeVST directory has an MIT license, with third-party components retaining their own licenses. It supports external RVC runtime, .pth inference models and optional .index files. Its documented block range is 20–1000 ms and its reported plugin latency is twice the block duration. The upstream RVC realtime GUI reports 170 ms end-to-end latency, or 90 ms with ASIO in the authors' setup; these are upstream claims, not Voice Studio measurements or a guarantee on Wave Link.

Important hardware caveat: the RVCRealtimeVST developer guide lists a verified PyTorch 2.7.1+cu118 runtime, but that is not the correct dependency set to copy blindly for this project's RTX 5060 Ti. The current upstream RVC README explicitly specifies cu128 for RTX 50-series. The prototype must validate the exact Python/PyTorch/Torchaudio/CUDA combination and model/F0 path on Blackwell before any dependency is pinned.

Sources:
- https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI
- https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI/blob/main/LICENSE
- https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI/tree/main/RVCRealtimeVST
- https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI/blob/main/RVCRealtimeVST/LICENSE.txt

Decision: use official RVC realtime inference as the first feasibility target, and study the RVCRealtimeVST worker/streaming path as a reference. Do not copy its iPlug2 plugin wholesale; Voice Studio remains JUCE. Do not adopt its runtime version pins without the RTX 50-series compatibility test.

### 2. Applio — alternative RVC distribution/runtime

Applio is an actively maintained RVC-focused suite with real-time conversion and model training/inference. Its repository declares MIT licensing and its documentation describes .pth inference exports with optional .index files. However, its desktop/WebUI packaging is broader than the minimal inference worker Voice Studio needs. It is a useful fallback source for inference implementation and dependency comparison, not the first integration target.

The project documentation says its source and included model weights are MIT licensed, but any externally downloaded community model must still be checked against its own publisher terms and voice rights.

Sources:
- https://github.com/IAHispano/Applio
- https://github.com/IAHispano/Applio/blob/main/LICENSE
- https://docs.applio.org/getting-started/installing-inference-models/

### 3. w-okada VCClient — reference only, not a dependency

VCClient demonstrates real-time conversion across RVC, so-vits-svc, MMVC and other engines, with Windows CUDA builds and configurable chunk/extra buffering. Its repository metadata reports an unspecified/NOASSERTION license, and supported model/corpus terms can differ. Do not copy or redistribute its code/assets unless the relevant licenses are clarified.

Sources:
- https://github.com/w-okada/voice-changer
- https://github.com/w-okada/voice-changer/blob/master/README_en.md

### 4. RVC Realtime (Rust/ONNX Runtime) and vc-rs — keep as later alternatives

- https://github.com/Ramo-Inc/rvc-realtime — a Windows Rust/ONNX Runtime/CUDA Graphs implementation; its published status is alpha and its model support is narrower than general RVC. Re-check its current license, runtime and bundled assets before use.
- https://github.com/shirohata/vc-rs — documents Windows RVC conversion with Windows ML and NVIDIA TensorRT options; useful for later deployment research, not adopted.

## Proposed first feasibility spike (not yet implemented)

1. Build a standalone, visible Python worker against the official RVC realtime inference path. It must not be launched automatically by the plugin.
2. Use Python 3.12 x64 and a tested PyTorch/Torchaudio CUDA 12.8 pair for RTX 50-series. Pin all transitive dependencies only after a clean Windows environment test; do not rely on a system Python installation.
3. Load one user-supplied, explicitly licensed RVC v2 F0 model. Keep model and index optional and separate from the application package. Never bundle a community model by default.
4. Feed deterministic audio blocks to the worker from a standalone harness first. Measure inference time per block, cold-start/model-load time, GPU memory, CPU use, audio continuity, and sustained operation. Test 20/40/60 ms block sizes rather than assuming the smallest block is stable.
5. Test worker stop, timeout, crash, restart, malformed input and stale output. If inference misses its deadline, discard stale converted audio and transition to the existing DSP/dry fallback; never replay seconds of queued speech.
6. Only after the harness passes, adapt the worker transport to the reviewed Voice Studio IPC contract. The VST audio callback must never wait for IPC, inference, locks, disk, or process lifecycle operations.

## Licensing and model policy

- MIT licensing of RVC source code does not grant rights to every third-party voice model, dataset, speaker identity, or recording.
- Every model must have a recorded source URL, publisher/license, SHA-256, model architecture/version, sample rate, F0 mode, and permission/voice-rights review.
- Do not download or install model assets automatically. No runtime network access from the plugin or worker.
- Keep third-party notices and dependency licenses with any distributed runtime. Review PyTorch, CUDA redistributables, audio libraries, and every transitive package before packaging.

## Required engineering gates before production integration

- Confirm Wave Link 3 VST3 host behavior and supported channel layouts on the target machine.
- Prove RTX 5060 Ti inference works with the pinned CUDA 12.8 runtime and a permitted model.
- Measure end-to-end latency, underruns, CPU/GPU use and recovery on the target hardware; do not repeat upstream latency claims as our own.
- Test operation while PUBG/OBS/Wave Link are active to detect GPU contention and stale queue buildup.
- Enforce no outbound network access at the worker/process boundary where practical; import-table auditing alone is insufficient.
- Add automated tests for ring overflow/underflow, stale blocks, worker crash, malformed IPC, invalid model files and dry fallback.
- Do not install or modify anything on the user's PC until a reviewed test package and installation plan are explicitly approved.

## Current decision

RVC is selected as the first *feasibility prototype target*, not yet as a production dependency. The next implementation step is a standalone RVC inference/latency harness using the RTX 50-series CUDA 12.8 dependency set. No model is bundled, no worker is started by the plugin, and no AI control is enabled until that spike proves stable real-time behavior on the target system.