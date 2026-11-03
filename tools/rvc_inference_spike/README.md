# RVC Inference Preflight

This tool checks a separately supplied RVC source checkout and its Python
runtime. It does not install packages, access the network, download models,
load a checkpoint, or perform voice conversion.

## Why this stage exists

The upstream RVC realtime VST implementation exposes an inference API through
`infer.rtrvc.RVC`. Its constructor accepts pitch/formant, checkpoint/index
paths and a Config object; its `infer` method accepts a 16 kHz input tensor,
block/context lengths and an F0 method. This preflight imports that API and
checks the expected method signatures before we write our own inference bridge.

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
- Whether the RVC Config selects CUDA rather than silently falling back to CPU.
- A strict readiness gate that rejects missing PyTorch/Torchaudio, mismatched package versions, unavailable CUDA, failed FP32/FP16 kernels, and incompatible RVC APIs.

The current RTX 50-series target gate expects a CUDA 12.8 PyTorch build.
A successful preflight still does not prove model compatibility or real-time
latency. Those require a compatible licensed model and a separate benchmark.

## Offline unit tests

    python -m unittest -v
