# Local AI Worker IPC Protocol (Draft v0.1)

Status: design specification only. This document does not add IPC code, launch a worker, download dependencies, or change the plugin's runtime behavior.

## 1. Goals and non-goals

The protocol connects the Voice Studio VST3 to an optional, separately running local AI Worker on Windows x64.

Required properties:
- AI is opt-in and disabled by default.
- No cloud service, telemetry, account sign-in, TCP/UDP socket, loopback networking, or automatic download.
- The VST3 audio callback never waits for IPC, inference, disk I/O, process creation, or a worker response.
- The existing DSP path remains available whenever the worker is unavailable or unhealthy.
- Messages and audio buffers are bounded and versioned.
- The worker is a normal user-mode process; no kernel driver, injection, or security bypass.

Out of scope: choosing an RVC engine, defining a model format, packaging runtimes, and enabling AI in the plugin.

## 2. Process and thread ownership

VST3 process:
- Audio callback: real-time thread owned by the host. It may only copy bounded samples/metadata to and from preallocated memory and perform existing DSP/fallback.
- Control thread: non-real-time plugin-owned thread. Owns IPC connection setup/teardown and Start/Stop requests.
- UI thread: displays state and sends user intent to the control thread. It does not access audio transport directly.

AI Worker:
- IPC/control thread: validates control messages and owns connection lifecycle.
- Audio ingest thread: drains input audio transport into worker-owned buffers.
- Inference thread(s): perform feature extraction and voice conversion; never run on the VST3 callback.
- Audio publish thread: publishes completed output blocks.

No thread may have more than one producer or one consumer on an SPSC queue. If multiple inference threads are used, they must not write directly into a single-producer output queue without an explicit serialization stage.

## 3. Transport layout (provisional)

Preferred direction for a Windows prototype:
- Shared memory for fixed-size audio slots.
- A narrowly scoped Windows named pipe for control and lifecycle messages only.
- No socket transport of any kind.

This is a design preference, not an implementation decision. The current in-process `SpscQueue` class is not itself a cross-process IPC primitive. Its C++ object layout, atomics, and memory-order assumptions must not be treated as a stable shared-memory ABI. A separate shared-memory layout must specify alignment, fixed-width fields, synchronization primitives supported across processes, initialization ownership, and crash recovery before code is written.

The control pipe must use a restrictive security descriptor so only the current user (and required system identity) can access it. Names must be unpredictable per session and must not expose user data. The worker must reject unexpected clients and protocol versions.

## 4. Audio format and block contract

Protocol v0.1 baseline:
- Sample representation: IEEE-754 32-bit float.
- Channel layout: explicit channel count and documented channel order; initial prototype should support mono and stereo only.
- Sample rate: explicit unsigned integer in Hz, negotiated at session start. No implicit resampling.
- Frames per block: explicit per block; bounded by negotiated maximum. The initial prototype should test common host block sizes and must not assume the host always supplies one fixed size.
- Sequence: monotonically increasing unsigned 64-bit block sequence per direction and session.
- Timestamp: monotonic session-relative frame position, not wall-clock time.
- Samples: planar or interleaved must be selected once for v1 and stated in the handshake. Do not infer layout from buffer size.
- Every slot includes protocol version, session identifier, sequence, frame count, channel count, sample rate identifier/value, flags, and payload length.
- Reject non-finite samples and invalid dimensions before inference; sanitize invalid output before it reaches the host.

Maximum frame count, maximum channel count, and total shared-memory size must be compile-time/configuration bounded. Any size calculation must be overflow-checked before mapping or indexing memory.

## 5. Latency, ordering, and stale data

- The VST3 callback performs only bounded, non-waiting slot operations.
- Input queue full: drop the newest input block and increment an overrun counter; never overwrite a slot the worker may still be reading.
- Output queue empty: use the defined dry/DSP fallback for that host block; do not wait.
- Output sequence older than the current playback deadline: discard it. Never play stale converted audio late merely to preserve every block.
- Sequence gaps are observable counters, not fatal protocol errors.
- The worker must publish output with the source input sequence so the plugin can detect late, duplicated, or out-of-order output.
- No unbounded buffering. Queue capacity is a measured latency/robustness trade-off and must be documented with its worst-case queued duration.
- The initial prototype must measure round-trip processing latency and underruns at the target host sample rates and block sizes before choosing production queue depth.

## 6. Control protocol

All control messages are length-bounded, versioned records. The decoder must reject unknown mandatory fields, invalid lengths, unsupported versions, and malformed values. A message must never cause arbitrary file execution or arbitrary path loading.

Initial message set:
- HELLO: protocol version range, worker build identity, supported audio formats, and capability flags.
- HELLO_ACK: selected protocol version and negotiated limits.
- CONFIGURE: sample rate, channel layout, block limits, and session identifier.
- CONFIGURE_ACK: accepted configuration or a bounded error code.
- START: explicit user-authorized request to begin processing.
- STOP: request to stop inference and return to DSP-only mode.
- STATUS: worker state and bounded diagnostic counters.
- ERROR: stable error code plus short, non-sensitive diagnostic text.
- SHUTDOWN: orderly worker exit request.

No control message may contain credentials, network endpoints, shell commands, executable paths, or arbitrary code. Model selection is a separate explicit user action and is not part of automatic session negotiation.

## 7. Lifecycle and state machine

Expected states:
1. Disabled: default after plugin load; no worker launch and no IPC connection.
2. Starting: entered only after the user presses Start AI; process launch and IPC setup occur on the control thread.
3. Handshaking: version and capabilities are negotiated.
4. Ready: configuration accepted; worker is available but conversion has not necessarily started.
5. Running: explicit Start accepted and audio blocks may be exchanged.
6. Stopping: STOP sent; plugin immediately uses DSP fallback while worker shuts down.
7. Faulted: timeout, crash, malformed message, incompatible version, or invalid output; plugin stays on DSP fallback.
8. Stopped: worker has exited or disconnected; no automatic restart.

No automatic worker launch on plugin load, host restart, or crash recovery. A fault must not trigger an infinite restart loop.

## 8. Timeouts and failure behavior

- The audio callback has no IPC timeout because it never waits.
- Control operations use finite deadlines and cancellable waits on the control thread.
- If handshake/configuration exceeds its deadline, mark the worker unavailable and remain in DSP mode.
- If the worker exits, the plugin invalidates the session and ignores any remaining output from that session.
- If output contains invalid dimensions, non-finite samples, an unknown session, or an impossible sequence, discard it and transition to Faulted after the configured error policy threshold.
- Worker restart requires a fresh explicit user action and a new session identifier.
- The fallback must not introduce a click: define and test a short bounded crossfade between converted and DSP/dry paths, without waiting for the worker.

## 9. Security and privacy requirements

- Local-only IPC; no TCP, UDP, loopback sockets, cloud, telemetry, or silent update checks.
- Worker runs with the same standard user privileges as the host; never request elevation.
- Restrict named-pipe and shared-memory object access to the current user/session.
- Use unpredictable per-session object names and clean them up on orderly shutdown; stale objects must not be trusted after a crash.
- Validate every message and all lengths before accessing shared memory.
- Model files are data only. Never load model-supplied DLLs, scripts, executables, or plugins.
- Model paths must be selected by the user; prevent path traversal and enforce file-size limits.
- Record model provenance, license, voice-rights confirmation, and SHA-256 before activation.
- Do not weaken Defender, firewall, SmartScreen, or other Windows security controls.
- Static import scans are not proof of runtime network inactivity. Verify behavior with source review and OS-level process/network observation during testing.

## 10. Observability and diagnostics

Expose bounded counters in the UI/log:
- input overruns;
- output underruns;
- stale output blocks;
- invalid block/message count;
- worker restarts (expected to remain zero unless user manually restarts);
- last known worker state;
- measured processing latency where available.

Diagnostics must not contain microphone samples, converted audio, model contents, personal file paths unless the user explicitly exports a diagnostic report, or network data.

## 11. Test gates before implementation

Protocol tests:
- valid and invalid handshake/version negotiation;
- malformed, oversized, truncated, and unknown messages;
- invalid dimensions and integer-overflow cases;
- session mismatch, duplicate/out-of-order sequence, and sequence wrap policy;
- shared-memory initialization and worker crash during read/write;
- queue full/empty, stale output, and bounded memory behavior;
- worker stop/crash during active audio;
- fallback continuity and click/pop behavior;
- no worker launch while AI is disabled;
- verify no TCP/UDP sockets and no outbound network activity in the worker during offline tests.

Performance tests:
- host sample rates and variable block sizes;
- mono/stereo;
- CPU/GPU load, latency distribution, underrun rate, and recovery;
- long-duration stability and repeated Start/Stop cycles.

Do not integrate an inference engine until these gates pass and its source, license, dependencies, runtime binaries, model format, and redistribution terms have been reviewed.

## 12. Open decisions

The following remain intentionally undecided:
- final shared-memory ABI and cross-process synchronization primitive;
- named-pipe message encoding (fixed binary header plus bounded payload is preferred; serialization library not selected);
- slot count and maximum block dimensions;
- planar versus interleaved sample storage;
- exact sample-rate support and resampling policy;
- worker implementation language and RVC engine;
- model format and GPU runtime packaging.

These decisions require a reviewed prototype and measurements. This document does not authorize installation or changes to the user's PC.
