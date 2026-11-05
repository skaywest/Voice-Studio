# RVC Inference Preflight

This tool checks a separately supplied RVC source checkout and its Python
runtime. The adjacent `model_selection.py` validates explicit user selections
and creates a SHA-256 manifest for the RVC worker entrypoint, model checkpoint,
and optional index. It requires explicit trust and voice-rights confirmations.
Neither tool installs packages, accesses the network, downloads models,
executes RVC code, deserializes a checkpoint, or performs voice conversion.

## Why this stage exists

The upstream RVC realtime VST implementation exposes an inference API through
`infer.rtrvc.RVC` and a streaming wrapper through
`RVCRealtimeVST/worker/rvc_worker.py` (`RVCStreamEngine`). The preflight checks
both APIs, including the streaming engine constructor and ordered block-process
signature. The streaming worker source is inspected with Python AST; it is not
executed by this API check. This validates the integration boundary without
loading a checkpoint or running inference.

## Run on a machine with an existing RVC runtime

PowerShell example:

    python .\preflight.py --python "D:\RVC\runtime\python.exe" --rvc-root "D:\RVC"

Optional files:

    python .\preflight.py --python "D:\RVC\runtime\python.exe" --rvc-root "D:\RVC" --model "D:\Models\voice.pth" --index "D:\Models\voice.index"

Strict target check:

    python .\preflight.py --python "D:\RVC\runtime\python.exe" --rvc-root "D:\RVC" --require-cuda

The optional model and index arguments only validate that the files exist and
have expected extensions. Checkpoints are deliberately not deserialized in
this stage. No model is required to validate the RVC source/API and GPU runtime.

## Checks

- Required upstream RVC source files and all five configuration profiles exist.
  The upstream `Config` constructor loads every profile during initialization.
- External Python executable starts and can import PyTorch, Torchaudio, NumPy,
  Librosa, FAISS and Parselmouth.
- CUDA availability, build version and real FP32/FP16 CUDA operations with synchronization.
- Explicit PyTorch/Torchaudio installation flags and full release-version matching (CUDA build suffixes are ignored).
- GPU name, compute capability and available memory.
- Actual imports of `configs.config` and `infer.rtrvc`.
- `RVC` constructor and `RVC.infer` signatures match the expected adapter API.
- The upstream `RVCStreamEngine` constructor and `process` block signature are present and compatible.
- Whether the RVC Config selects CUDA rather than silently falling back to CPU.
- A strict readiness gate that rejects missing PyTorch/Torchaudio, mismatched package versions, unavailable CUDA, failed FP32/FP16 kernels, and incompatible RVC APIs.

The current RTX 50-series target gate expects a CUDA 12.8 PyTorch build.
A successful preflight still does not prove model compatibility or real-time
latency. Those require a compatible licensed model and a separate benchmark.

## Offline unit tests

    python -m unittest -v

## Explicit model/source selection manifest

The offline helper `model_selection.py` is a preparation step for a future
Worker launch. It requires both `trust_rvc_source=True` and
`confirm_voice_rights=True`, checks the selected RVC worker entrypoint is
inside the chosen source directory, validates non-empty `.pth` / optional
`.index` files, and fingerprints them with streaming SHA-256. Its verifier
detects changed files before a later activation step.

This is not a sandbox or a proof that an RVC checkout is safe. The selected
RVC checkout contains executable Python and must be trusted by the user;
checkpoints may use pickle-based formats and are not deserialized here. The
manifest is not yet connected to the native Worker or a UI. No files are
launched or loaded by this helper.

## Offline Worker runtime manifest builder

The experimental `runtime_manifest.py` helper inventories an explicitly
selected runtime tree and emits the same canonical SHA-256 manifest format
used by the native runtime-manifest ABI:

```powershell
python .\runtime_manifest.py --root "D:\RVC\runtime-bundle" --output ".\runtime-manifest.json"
```

The tree must be prepared locally and contain the exact files intended for
staging: RVC Python sources, checkpoint, optional FAISS index, selected HuBERT
and F0 assets, configuration, and required runtime support files. Paths are
restricted to portable ASCII relative paths; symlinks, Windows device-name
aliases, empty files, duplicate/case-colliding paths, and trees exceeding the
native ABI limits are rejected. The current ABI permits at most 512 files,
8 GiB per file, and 32 GiB total.

This helper only inventories and hashes files. It does not execute Python,
install dependencies, download anything, authenticate a publisher, or prove
that a runtime is safe. Its JSON digest is an integrity checksum, not a
digital signature. The native Worker must independently verify the manifest
and every staged file. Runtime transfer and inference are not yet wired to
this helper; the Worker continues to reject `start` until the audited backend
and complete runtime-loading path are implemented.
