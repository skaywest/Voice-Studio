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
