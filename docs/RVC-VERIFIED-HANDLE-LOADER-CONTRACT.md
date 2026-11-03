# RVC verified-handle loader contract

Status: design gate. No loader implementation or inference runtime is included.

## Security boundary

The Worker verifier owns the root, entrypoint, model, and optional index handles
after successful manifest verification. The loader must consume the exact file
objects that were verified. It must not reopen a caller-provided path after
verification, and it must not accept a path from the plugin as an authority.

The current `VerifiedModelFiles` object is Worker-owned and non-copyable. Its
lifetime must cover model loading and every operation that may lazily read model
or index data. Unload and Worker shutdown release the handles only after all
backend tasks have stopped.

## Backend interface requirements

A future backend boundary should be equivalent to:

- `load(VerifiedModelFilesView, RuntimeOptions)` — no filesystem path arguments;
- `process(inputBlock, parameters)` — fixed-size, stateful stream operation;
- `stop()` — stop accepting blocks and join/cancel backend work off the audio thread;
- `unload()` — release model, index, runtime, and device resources.

The view may expose read-only duplicated Windows handles or bounded read
operations. It must not expose mutable handles, permit write/delete access, or
transfer ownership implicitly. Every duplicate must be closed by its receiver.
The Worker retains its original handles until unload completes.

## Important compatibility problem: upstream RVC is path-oriented

The inspected upstream realtime engine is Python and may call PyTorch, FAISS,
or other libraries that expect filesystem paths or reopen files lazily.
Passing a verified Windows HANDLE to Python does not automatically make those
libraries consume the same file object. Do not claim handle safety merely
because a Python wrapper received a path string.

Before integration, inspect every model/index load call and choose one of these
explicitly tested approaches:

1. Use a supported file-object/stream API end-to-end, proving that no lazy
   reopen occurs and that the native library reads the supplied object.
2. Create a private Worker-owned staging copy from the verified handle into a
   directory with restrictive ACLs, hash the staged bytes again, prevent
   replacement for the entire model lifetime, and pass only that staged path
   to path-only libraries. Cleanup occurs after backend unload.
3. Reject the backend/library combination if neither approach can preserve the
   integrity guarantee.

The entrypoint has the same issue. Executing a script by its original path
reopens it. Either load verified source bytes into a controlled Python module
execution path, or stage and re-hash the script under the same private-directory
policy. Do not execute an unverified path after checking a different handle.

## Required tests before enabling inference

- Verify a valid model and confirm that write/delete/replacement attempts fail
  while the Worker retains the verified handles.
- Verify wrong size, wrong SHA-256, missing file, directory instead of file,
  reparse-point root, reparse-point model, and a file resolving outside root.
- Verify the loader consumes bytes from the verified handle or from a staged
  copy whose digest is independently checked.
- Race replacement attempts against verification and loading; the loaded
  digest must always equal the manifest digest or loading must fail.
- Verify unload stops all readers before handles/staging files are released.
- Verify model/index/entrypoint failure leaves AI unavailable and DSP/dry
  fallback active.

## Non-goals

This contract does not implement or claim RVC inference, CUDA support, a model
download, or training. AI remains unavailable until the backend and its
runtime/model combination pass the tests above on Windows.


## Concrete upstream RVC source audit (2026-11-04)

The inspected upstream `RVCRealtimeVST/worker/rvc_worker.py` and
`infer/rtrvc.py` confirm that the current adapter boundary is path-based:

- `RVCStreamEngine.__init__` resolves `model_path` and `index_path` to
  strings and passes them to `infer.rtrvc.RVC`.
- `infer.rtrvc.get_synthesizer` calls
  `torch.load(pth_path, map_location=torch.device("cpu"))`.
- `RVC.__init__` calls `faiss.read_index(index_path)` when index search
  is enabled.
- `RVC.change_index_rate` may call `faiss.read_index(self.index_path)`
  later when index search is enabled after startup. Verifying only the
  initial read would therefore be insufficient.
- The worker also loads HuBERT from the RVC checkout's
  `assets/hubert_base` directory through Transformers
  `from_pretrained(path, local_files_only=True)`.
- RMVPE is loaded lazily from `assets/rmvpe/rmvpe.pt` when that F0 method
  is first selected.

Consequences for this project:

1. Direct verified-handle integration with the inspected upstream RVC
   implementation is **not currently established**. The checkpoint and FAISS
   index are explicitly reopened by path; the index can be reopened later.
2. A staging-copy design must include the entrypoint, checkpoint, optional
   index, and every runtime asset actually selected (including HuBERT and
   RMVPE when used). The staged tree needs a private ACL, independent hashes,
   no user-writable replacement path, and a lifetime extending until backend
   unload. Any lazily loaded asset must be staged and verified before the
   method is enabled, or loaded through a separately verified read-only
   mechanism.
3. The current model manifest/ABI covers entrypoint, `.pth`, and optional
   `.index` only. It does **not** yet authenticate the complete RVC source
   tree, Python runtime, package set, HuBERT assets, RMVPE assets, or other
   optional F0 assets. It must not be described as a complete trusted-runtime
   manifest.
4. The selected RVC source is executable Python. User confirmation of source
   trust remains necessary, but is not a substitute for staging integrity or
   process isolation.
5. Before enabling inference, add tests that prove the backend never reads
   original selected paths after staging, verify hashes for all staged files,
   and exercise a late index activation and each supported F0 model load.

The repository's current Worker remains a protocol/audio echo Worker:
`start` is unsupported. No RVC code is executed, no checkpoint is
deserialized, and no GPU inference is claimed by this audit.

## Staging design decision (2026-11-04)

Decision: use a Worker-owned staging tree for the inspected path-oriented RVC backend. Do not pass originally selected checkout/model paths to Python, PyTorch, FAISS, Transformers, or the RVC adapter.

### Staging transaction

1. The Worker accepts a versioned, bounded manifest. Each entry has a normalized relative path, exact byte length, SHA-256, and a role (Python source, checkpoint, FAISS index, HuBERT asset, F0 asset, or runtime support file). Absolute paths, drive prefixes, UNC paths, alternate data stream syntax, dot segments, duplicate/case-colliding paths, and reparse points are rejected.
2. The Worker creates a fresh unpredictable staging directory beneath an application-owned data root. It must not reuse an existing directory. The directory and files are created exclusively (no overwrite); inherited permissive ACLs are replaced before content is written. The DACL grants access only to the Worker identity and required operating-system principals.
3. Source files are opened by the existing verifier, then copied from those verified handles—not reopened by caller-supplied names. The copy loop computes SHA-256 while writing. The staged file is flushed, closed, reopened read-only, and independently hashed before acceptance. Size and digest must match at both source-verification and staged-verification boundaries.
4. The staged tree is immutable for the backend lifetime. No write/delete handles are retained or shared; files are not writable by the plugin. The Worker passes only canonical staged paths to path-based RVC APIs. Delayed index activation and lazy F0 loading must resolve only within this same staged tree.
5. Backend shutdown first stops new audio work, then waits for inference and lazy asset readers to finish, unloads FAISS/PyTorch/model objects, and only then removes the staging tree. A failed stage/load removes the partial tree and leaves AI unavailable with DSP/dry fallback.
6. The Worker records the staged manifest digest and rejects any file whose path, size, or digest is not in the accepted manifest. No network fetch, package installation, or model download is part of this transaction.

### Manifest coverage

The manifest must cover the complete executable/runtime closure, not only the three current ABI paths:

- the exact RVC source revision and every imported Python source/module file;
- the realtime entrypoint and adapter source;
- the .pth checkpoint and optional .index file;
- the complete selected HuBERT directory and selected F0 assets (including RMVPE when enabled);
- configuration JSON and any other data files opened at runtime;
- the Python executable, standard-library/runtime files, and native extension modules required by the pinned package set.

The initial implementation may deliberately support a smaller, explicit feature set (for example one pinned F0 method and no optional index). It must fail closed when a selected feature requires an asset absent from the manifest. It must not silently fetch missing files or fall back to an unverified location.

### Threat model and limits

A restrictive ACL and exclusive creation prevent ordinary plugin/user-level replacement when the Worker runs under a distinct, appropriately restricted identity. If the Worker and an attacker run with the same Windows identity, that attacker may still have equivalent access to the staging directory; same-user ACLs alone are not a security boundary against a malicious process with the same token. The product must either document that limitation or run the Worker under a restricted identity with a separately protected data root. Administrators and kernel-level attackers are outside this file-integrity boundary.

The source checkout and Python packages are executable code. A user trust confirmation is required for an externally selected RVC source/runtime, but confirmation does not authenticate it. Production should prefer a pinned, application-supplied offline runtime and source bundle with a verified release manifest. Until such a bundle exists, external runtime support remains explicit opt-in and must not be described as a fully trusted/sandboxed runtime.

### Implementation gates

- Extend the native manifest ABI with a bounded list of relative file entries and a manifest version; keep current fixed-size message limits explicit.
- Implement safe directory creation and ACL verification before writing any staged bytes.
- Implement handle-based source copying, exclusive destination creation, flush/reopen/re-hash, and complete cleanup on every failure path.
- Add Windows tests for traversal, duplicate paths, case collisions, reparse points in every path component, pre-existing destination files, ACL mismatch, source mutation during copy, staged mutation attempts, and cleanup after failed copy/load.
- Add adapter tests that observe every file open and prove that startup, late index activation, and each enabled F0 method use staged paths only.
- Do not enable start until these tests pass and a real model-backed CUDA inference/latency test is run. CI without PyTorch/CUDA can validate the staging protocol but cannot claim inference readiness.
