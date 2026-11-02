# Windows Shared-Memory Audio Transport (Design Draft v0.1)

Status: design proposal only. No Windows IPC code, worker process, named object, dependency, or runtime behavior is added by this document.

This draft narrows the open shared-memory ABI decision from `LOCAL-AI-WORKER-IPC-PROTOCOL.md`. It is intentionally not a promise of lock-free behavior until verified on the supported Windows x64 toolchain and tested across two real processes.

## 1. Scope and invariants

- Windows x64, same interactive user session, same standard-user privilege level.
- One VST3-side producer and one worker-side consumer for the input ring.
- One worker-side producer and one VST3-side consumer for the output ring.
- Two independent SPSC rings; never share a ring between multiple producers or consumers.
- Audio callback performs bounded memory copies and atomic slot checks only. It never creates/opens/maps objects, waits, retries in a loop, allocates, logs, or calls a system API.
- The control thread owns object creation/opening, mapping, validation, lifecycle, and cleanup.
- A worker crash or malformed slot must result in dry/DSP fallback, not a wait in the callback.
- No TCP, UDP, loopback socket, cloud, telemetry, automatic download, or privilege elevation.

## 2. Windows object model

Proposed objects, created only after an explicit user action:

1. A page-file-backed file mapping for the session audio transport.
2. A named pipe for bounded control/lifecycle messages only.

Names must be generated from a cryptographically strong per-session nonce and include a product-specific prefix. Names must not contain the user's name, microphone/device name, model path, or other personal data.

The control thread creates the objects with an explicit security descriptor granting access only to the current user SID and the minimum required system identity. Do not use a permissive default DACL. The worker must verify the peer/session identity and reject unexpected clients. Object names are not treated as authentication.

The mapping must be created with a fixed maximum size. The worker must not be allowed to resize it or supply an arbitrary mapping size. The control handshake carries the expected protocol version, mapping size, session nonce, and layout hash; all must match before audio exchange begins.

## 3. ABI rules

- Fixed-width integer fields only: `uint32_t`, `uint64_t`, and byte arrays.
- No pointers, `size_t`, STL containers, virtual objects, compiler-dependent bitfields, or C++ `std::atomic` objects in the mapped ABI.
- All offsets are from the beginning of the mapping and are validated before dereference.
- Header and slot structures have explicit alignment and compile-time size/offset assertions in each process.
- ABI version changes whenever field meaning, alignment, synchronization, or payload layout changes.
- Little-endian Windows x64 is the only supported byte order in this draft.
- The mapping contains no executable code, handles, credentials, paths, or model data.

## 4. Proposed mapping layout

The mapping consists of:
- A fixed header page.
- An input ring region.
- An output ring region.

The header records:
- magic bytes and ABI major/minor version;
- header size and total mapping size;
- session nonce (fixed-size opaque bytes);
- sample rate in Hz;
- channel count (1 or 2);
- sample representation identifier (IEEE-754 float32);
- sample layout identifier (planar in this draft);
- maximum frames per slot;
- slot count;
- input and output region offsets and region byte lengths;
- layout hash / feature flags;
- initialization state and reserved zeroed bytes.

Each ring has a cache-line-aligned control area and a fixed array of slots. Each slot contains:
- an aligned 64-bit sequence/state word used for ownership publication;
- source audio sequence number;
- session-relative first-frame timestamp;
- frame count and channel count;
- payload byte count;
- flags/reserved fields (must be zero unless defined by this ABI);
- fixed-capacity planar float32 payload.

No variable-sized allocation occurs inside the mapping. Unused payload bytes are ignored and should be initialized to zero during session setup.

## 5. Cross-process synchronization proposal

Do not place `std::atomic` in shared memory and assume its representation or process-sharing semantics are portable. For the Windows-only prototype, use naturally aligned 64-bit Windows Interlocked operations on the slot sequence word, with compile-time alignment checks and runtime platform checks.

Proposed bounded SPSC slot protocol for a ring of capacity N:

- Initialization: the creator initializes slot i's sequence word to i before publishing the mapping as ready.
- Producer at logical position p: read the slot sequence using an Interlocked compare/exchange read operation. The slot is writable only when its sequence equals p. If it does not, return immediately as full; do not spin.
- Producer copies the bounded metadata and payload, then publishes the slot by setting sequence to p+1 with an Interlocked exchange operation.
- Consumer at logical position c: the slot is readable only when its sequence equals c+1. If it does not, return immediately as empty; do not spin.
- Consumer validates metadata and copies the bounded payload, then releases the slot by setting sequence to c+N.
- Producer and consumer maintain private 64-bit logical positions in their own process. They are not shared mutable counters.
- Each direction has its own independent sequence space.

This sequence protocol is a proposal and must be verified with a two-process stress test. Interlocked operations provide the required cross-process atomicity/order on supported Windows systems, but the exact chosen API, compiler declarations, alignment, and memory barriers must be reviewed before implementation. If the target cannot guarantee aligned 64-bit interlocked operations, the prototype must fail closed rather than silently use a potentially locking fallback.

The callback-side operation has a fixed number of atomic operations and one bounded copy. It must never retry on contention. The control/worker side may do bounded work outside the callback, but must not hold a slot while running inference.

## 6. Slot and payload validation

Before copying or processing a slot, validate:
- session nonce and ABI version;
- slot sequence and expected direction;
- frame count is within 1..maxFrames;
- channel count matches negotiated mono/stereo layout;
- sample rate matches the session;
- payload byte count equals frames × channels × sizeof(float), with checked multiplication;
- all reserved fields are zero;
- timestamp and source sequence follow the session's monotonic policy;
- sample values are finite before inference and before publishing output.

Any failed validation increments a bounded error counter, discards the slot, and transitions the session to Faulted according to the protocol policy. Never trust offsets, sizes, or pointers received from the worker.

## 7. Initialization, teardown, and crash recovery

- The creator initializes the complete mapping while the session is not visible as Ready.
- The control pipe handshake completes before the mapping is marked active.
- A new random session nonce is used for every start; a stale mapping is never reattached automatically.
- On STOP, the plugin immediately switches to DSP fallback. The control thread requests worker shutdown and waits only with a finite deadline.
- On timeout/crash, invalidate the session locally, stop consuming output, close handles on the control thread, and leave the plugin in DSP fallback.
- Do not attempt to repair or reinitialize a ring while either process might still access it.
- The next attempt requires a fresh explicit user action and newly created objects.
- Cleanup must handle partial initialization and process exit without deleting unrelated objects.

## 8. Capacity and latency

Do not choose a production slot count from guesswork. For each candidate capacity, calculate the maximum queued audio duration:

`queued_ms = slot_count × max_frames_per_slot × 1000 / sample_rate`

Measure actual host block sizes and worker processing latency at 44.1, 48, and 96 kHz where supported. Test variable host block sizes, CPU/GPU contention, and long-duration operation. A queue that prevents drops by adding unacceptable latency is not acceptable.

The audio callback never waits for a free input slot or available output slot. Input full means drop newest input. Output empty or stale means fallback for that host block. Do not replay late converted audio.

## 9. Required test gates before implementation

A standalone Windows x64 test executable (not the plugin) must prove:

1. Header size, offsets, alignment, and layout hash are identical in both processes.
2. Creator/worker handshake rejects wrong version, wrong nonce, wrong mapping size, and wrong layout hash.
3. One million or more blocks cross the process boundary with correct ordering, metadata, and sample payload.
4. Full and empty conditions return immediately without overwriting unread data.
5. Producer and consumer process termination at every ownership transition does not deadlock or expose partially published blocks.
6. Repeated start/stop creates fresh sessions and does not attach to stale mappings.
7. Malformed dimensions, byte counts, reserved fields, sequence values, and NaN/Inf samples are rejected.
8. Variable block sizes and mono/stereo payloads remain within fixed memory bounds.
9. Sanitizer/static analysis where available, plus Windows Application Verifier or equivalent handle/memory checks.
10. Callback-facing ring operations have bounded execution and no heap allocation, mutex, wait, logging, or system call.
11. Offline observation confirms the worker opens no TCP/UDP sockets and produces no outbound traffic.
12. A worker crash during active processing causes immediate DSP fallback without host audio-thread blocking.

Only after these tests pass should a minimal transport prototype be considered for integration. The existing in-process `SpscQueue` remains a separate test primitive and must not be cast onto or reused as this mapped ABI.

## 10. Open review questions

- Confirm exact Interlocked read/publish/release API and memory-order proof for the target MSVC version.
- Confirm whether a page-file-backed mapping plus named pipe is the preferred Windows object pair.
- Select fixed maximum frames and slot count only after measuring Wave Link/VST3 host callback sizes.
- Decide whether a mapping header uses one page or a smaller fixed aligned structure.
- Review DACL construction, pipe client verification, and per-session name generation.
- Decide whether output should be dry or existing DSP path on underrun; current product behavior favors existing DSP.
- Confirm planar layout with the eventual inference engine before freezing ABI v1.

No inference engine, runtime, model, installer, or user-PC change is authorized by this draft.
