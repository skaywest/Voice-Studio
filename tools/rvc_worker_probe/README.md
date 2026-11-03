# RVC Worker Environment Probe

This is a standalone offline diagnostic. It is not the AI Worker and does not
perform voice conversion or load an RVC model.

## Target runtime candidate

- Windows x64 and Python 3.12 x64.
- A pinned PyTorch + Torchaudio pair built for CUDA 12.8 (cu128).
- PyTorch 2.7.1 / Torchaudio 2.7.1 is a documented candidate, not yet a
  validated final dependency choice for the RTX 5060 Ti or RVC.
- The exact runtime must be validated on the target PC before the Worker uses it.

## Run

From this directory in PowerShell:

    py -3.12 -m venv .venv
    .\.venv\Scripts\Activate.ps1
    python probe.py
    python probe.py --json
    python probe.py --require-cuda

The probe does not install packages, download dependencies, access the network,
or load a voice model.

## What it checks

- Python 3.12 and 64-bit process.
- PyTorch installed and CUDA build version.
- Torchaudio installed and major/minor version aligned with PyTorch.
- CUDA device discovery and device properties.
- Free/total GPU memory when the runtime exposes it.
- A real CUDA FP32 matrix operation followed by device synchronization and
  finite-result validation.
- A separate CUDA FP16 matrix operation and synchronization.

Exit codes:

- 0: diagnostic completed; with --require-cuda, all required checks passed.
- 2: --require-cuda was requested but any required Python, package, CUDA,
  kernel, or FP16 check failed.

A passing probe only validates the Python/GPU runtime. It does not prove RVC
model compatibility, real-time latency, or voice conversion. Those require a
licensed model and a separate inference harness/benchmark.

## Tests

    python -m unittest -v
