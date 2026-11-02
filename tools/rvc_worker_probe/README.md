# RVC Worker Environment Probe

This is a standalone offline diagnostic. It is not the AI Worker and does not
perform voice conversion.

## Requirements

- Windows x64 is the target platform.
- Python 3.12 x64 is the intended runtime.
- For the RTX 50-series feasibility target, validate a PyTorch build using
  CUDA 12.8 (cu128).
- PyTorch is optional for running unit tests, but required for a meaningful
  CUDA runtime check.

## Run

From this directory in PowerShell:

    py -3.12 -m venv .venv
    .\.venv\Scripts\Activate.ps1
    python probe.py
    python probe.py --json
    python probe.py --require-cuda

The probe does not install PyTorch. Install a reviewed, pinned local runtime
separately when the dependency set is finalized. Do not use this probe to
download dependencies or models.

Exit codes:
- 0: diagnostic completed; with --require-cuda, all required checks passed.
- 2: --require-cuda was requested but Python 3.12 x64, PyTorch, CUDA
  availability, or a CUDA 12.8 PyTorch build is missing.

model_loaded is always false. A successful CUDA check is not proof that RVC
inference works; a separately licensed model and an inference benchmark are
still required.

## Tests

    python -m unittest -v
