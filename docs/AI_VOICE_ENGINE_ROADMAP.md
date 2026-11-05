# AI Voice Engine Roadmap

This document tracks the next implementation stage for Voice Studio. It is a plan, not a claim that AI conversion is already available.

## Current verified boundary

- The VST3 and visible Worker/IPC foundation exist on `experiment/no-network-juce`.
- The Worker currently validates transport/lifecycle and does not perform inference.
- The RVC preflight and realtime API inspector are diagnostic tools only. They do not install dependencies, execute an RVC checkout, load checkpoints, or convert audio.
- The model catalog is intentionally empty.
- The native audio stream adapter uses bounded fixed-size blocks; the audio callback must never wait for inference or perform disk/network/model loading.

## Goals

1. Add a genuine local AI voice-conversion engine behind the existing Worker boundary.
2. Keep the VST3 usable when the AI Worker is stopped, unavailable, late, or fails.
3. Make model installation explicit, inspectable, reversible, and integrity-checked.
4. Keep audio private: no telemetry, account, cloud inference, or background network activity.
5. Ship only dependencies and models whose provenance, license, redistribution rights, and hashes have been reviewed.

## Implementation gates

### Gate A — Runtime decision and threat model

- Compare a supported, pinned local inference runtime against the existing RVC integration spike.
- Prefer a narrowly packaged runtime over launching an arbitrary user-selected Python environment.
- Treat pickle-based checkpoints and third-party Python source as executable/untrusted input; never silently load them.
- Record supported Windows x64, CUDA, driver, runtime, and model combinations.
- Document the exact trust boundary and failure behavior before enabling inference.

### Gate B — Worker inference prototype

- Implement inference only in the separate visible Worker process.
- Keep model loading, GPU initialization, allocation, and teardown off the VST3 audio thread.
- Use the existing versioned IPC and bounded audio transport; reject malformed messages and stale generations.
- Add explicit START, STOP, LOAD, UNLOAD, READY, ERROR, and timeout transitions.
- On underflow, crash, timeout, or invalid output, fall back to the existing DSP/dry path without blocking the host.
- Do not claim real-time performance until measured end-to-end latency, underruns, CPU, and VRAM are benchmarked.

### Gate C — Model library

- Add a catalog schema with stable IDs, description, language, model/runtime versions, file sizes, SHA-256, canonical source, publisher, license, and voice-rights provenance.
- Display all metadata and required disk/VRAM space before the user confirms installation.
- Downloads, activation, updates, and deletion must be user initiated.
- Reject unknown licenses, missing hashes, path traversal, unexpected file types, and unverified voice likeness rights.
- Never accept model packages containing installers, scripts, DLLs, drivers, or executables.

### Gate D — Product integration

- Add an AI Voices page with model status, install/remove, load/unload, voice selection, and clear Worker status.
- Keep AI disabled by default and make its state visible.
- Save user presets and restore them safely without automatically starting the Worker.
- Add accessibility-friendly labels and keyboard navigation.

## Required verification

- Unit tests for model manifests, hash mismatch, path traversal, invalid metadata, and license/rights gates.
- Worker lifecycle, timeout, crash, stale-block, and fallback tests.
- Audio tests for NaN/Inf rejection, channel conversion, queue overflow/underflow, and host callback sizes.
- Repeated Windows x64 Release build, CTest, network-import audit, and pluginval strictness 5.
- A model is not marked installable until source, license, voice rights, hashes, runtime compatibility, and security review all pass.

## Initial voice families

The intended library may offer original/licensed voice models across male, female, deep, bright, character/stylized, and multilingual categories. Classic DSP effects such as radio, robot, telephone, megaphone, distortion, echo, and pitch/formant remain available independently of AI.

No celebrity, streamer, actor, or other identifiable-person imitation is included without documented permission.
