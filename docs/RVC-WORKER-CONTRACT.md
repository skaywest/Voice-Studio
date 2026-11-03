# RVC Worker Contract (Design Gate)

Status: design only. No inference worker is implemented or launched by the VST3.

## Scope and non-goals

The first backend target is the official RVC realtime inference path. The worker is a separate, visible, user-started process. It must not be started by plugin construction, host scanning, project loading, or an AI parameter restore. No model or runtime is bundled, downloaded, or silently installed.

This contract does not claim that RVC runs on the target GPU. The RTX 5060 Ti path still requires a clean Windows validation with a reviewed Python 3.12 x64 / PyTorch CUDA 12.8 runtime and a user-supplied, licensed RVC v2 F0 model.

## Audio-thread invariants

The VST3 audio callback:

- never waits for a worker, event, pipe, mutex, condition variable, disk, model load, or inference;
- never allocates, starts/stops a process, or performs model lifecycle work;
- reads only a completed output block whose sequence and format metadata match the requested input block;
- discards stale, malformed, late, or non-finite output;
- uses the existing DSP path (or dry signal if DSP is bypassed) whenever converted audio is unavailable;
- does not accumulate a speech backlog. A late block is dropped, not played seconds later.

The worker may block, allocate, load models, and perform inference because it is not the audio callback.

## Block format and timing

The existing `WindowsSharedMemoryAbi.h` and `WindowsSharedMemoryPocTest` are a transport proof of concept only: they use eight slots of 64 float samples to test cross-process ownership, validation, corruption, crash detection, and restart. They are **not** a production audio-block format and must not be connected directly to live microphone audio.

A production audio-block ABI must carry, at minimum:

- ABI magic, version, header size, slot count, slot size and reserved-zero validation;
- monotonically increasing 64-bit sequence number and stream generation;
- sample rate, channel count, frame count, sample format and flags;
- input/output timestamps or a monotonic deadline, plus the sequence being converted;
- bounded audio payload capacity for the configured maximum block size and channel count;
- explicit slot ownership/state with acquire/release ordering and no reuse while a consumer owns a slot.

The first benchmark matrix is 20/40/60 ms at 48 kHz (960/1920/2880 frames), mono and stereo. The host's actual block size may differ; the adapter must accumulate/split blocks without waiting in the callback and must keep its queue bounded. These are benchmark targets, not a promise of achievable latency.

## RVC engine block adapter

The reviewed upstream realtime worker accepts a mono waveform and requires each
inference request to contain exactly its configured `block_frame` samples. Its
input resampler, HuBERT/F0 context, output resampler and SOLA overlap are stateful
across calls. The adapter must therefore preserve one ordered stream; it must
not invoke inference independently on arbitrary host callback buffers.

The first integration profile will use a 48 kHz host stream and a 60 ms RVC
block (2,880 mono samples). This fits the current shared-memory ABI limit of
2,880 frames. The worker configuration must explicitly set this block size;
the upstream default of 130 ms does not fit the current ABI. Do not silently
truncate or split one RVC inference request into unrelated model calls.

The plugin-side realtime adapter must:

- accumulate host callback samples into fixed 60 ms mono input blocks using
  preallocated bounded storage;
- downmix stereo input deterministically before publishing to the worker;
- consume converted mono output in stream order and distribute it across host
  callbacks, with a measured fixed latency budget;
- duplicate the converted mono signal to stereo outputs when required;
- drop an entire AI block on queue overrun or missed deadline, reset alignment
  safely on stream-generation changes, and retain DSP/dry fallback;
- perform no allocation, lock, wait, IPC, resampling, or inference in the audio
  callback; only bounded copies, simple arithmetic and lock-free publication
  are allowed there.

The initial profile is deliberately limited to 48 kHz and 60 ms blocks. Other
sample rates and block durations require explicit capacity, resampler and
latency tests before they can be enabled. The 20/40/60 ms benchmark matrix
remains a later measured optimization, not an assumption that all profiles
already work.

## Sequence, deadline and fallback policy

1. The host publishes an input block with a new sequence and stream generation into a free slot. If no slot is free, it drops the new AI block and keeps the normal DSP/dry output.
2. The worker validates the complete header, slot metadata, frame count, channel count and finite input samples before inference.
3. The worker publishes output only for the same sequence and generation, and only after the entire output payload is ready.
4. The host accepts output only if the sequence/generation match the expected block, metadata match the active stream, and the output is complete and finite.
5. Output that misses its deadline is discarded. The host must not wait for it or replay it later.
6. Worker exit, crash, timeout, malformed data, model-load failure or device loss moves the AI path to unavailable and leaves DSP/dry processing active.
7. Restart requires a new stream generation so output from an earlier worker instance cannot be mistaken for current audio.

The exact deadline and buffering budget must be measured on the target host; do not infer it from upstream RVC latency claims.

## Lifecycle state machine

- `Stopped`: no worker and no model loaded.
- `Starting`: explicit user action only; worker handshake is bounded and occurs off the audio thread.
- `Ready`: worker reports validated runtime, loaded model metadata and matching stream ABI.
- `Running`: blocks may be submitted and accepted under the sequence/deadline rules.
- `CoolingDown`: AI was disabled; retain the model for at most 30 seconds, then unload.
- `Unloading`: explicit `UNLOAD AI NOW` or cooldown expiry; stop accepting new work and release model/runtime resources.
- `Failed`: worker or runtime failed; immediately use DSP/dry fallback. A retry requires explicit user action.

Restoring a saved project must never launch the worker. The persisted AI preference may be restored as off until the user explicitly starts a compatible worker/model session.

## Worker control and observability

Control messages are separate from audio payload transport. They must be versioned, bounded, validated, and include request IDs. Required commands/status include START, STOP, UNLOAD_NOW, PING, READY, ERROR and STOPPED. No command may make the audio callback wait.

The visible worker reports runtime versions, selected device, model path, model SHA-256, model metadata, load duration, per-block inference duration, deadline misses, dropped/stale blocks, and worker restarts. Do not log microphone samples or transmit diagnostics.

## Validation gates before production integration

- Unit-test every ABI field, slot ownership transition, wraparound, sequence mismatch, stale generation, malformed metadata, NaN/Inf payload and capacity boundary.
- Process-level test worker exit/crash during transfer, timeout, restart and stale output from the previous generation.
- Benchmark 20/40/60 ms blocks for sustained runs, cold model load, warm inference, GPU memory, CPU use, deadline misses and audio continuity.
- Validate the exact RTX 5060 Ti / CUDA 12.8 / PyTorch / Torchaudio / RVC dependency combination on a clean Windows environment.
- Validate in Wave Link 3 with the real host block sizes and with OBS/PUBG active; measure rather than assume latency.
- Confirm the model's publisher, license, SHA-256, architecture, F0 mode, sample rate and voice/likeness rights before loading it.
- Keep network access out of both plugin and worker. The existing import-table audit is useful but is not proof against dynamically resolved networking APIs.

Until all gates pass, the AI control remains visibly unavailable and the current DSP path remains independently usable.
