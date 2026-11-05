"""Fixed-block contract between Voice Studio IPC and upstream RVC realtime.

The current native transport is mono float32, 48 kHz, 2880 frames (60 ms).
The upstream RVCStreamEngine accepts a configurable block_ms and computes
block_frame on 10 ms boundaries. This module builds that configuration and
checks the live engine/result at the Python Worker boundary.
"""
from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Iterator, Protocol

HOST_SAMPLE_RATE = 48_000
HOST_BLOCK_FRAMES = 2_880
HOST_CHANNELS = 1
HOST_BLOCK_MS = 60.0
RVC_CROSSFADE_MS = 40.0
RVC_EXTRA_MS = 2_000.0


class AudioBuffer(Protocol):
    """Sized, iterable audio storage (for example a NumPy array)."""

    def __len__(self) -> int: ...

    def __iter__(self) -> Iterator[float]: ...


class StreamContractError(RuntimeError):
    """The selected RVC engine cannot honor the native audio-block ABI."""


def expected_rvc_block_frames(sample_rate: int, block_ms: float) -> int:
    if isinstance(sample_rate, bool) or not isinstance(sample_rate, int) or sample_rate <= 0:
        raise ValueError("sample_rate must be a positive integer")
    if isinstance(block_ms, bool) or not isinstance(block_ms, (int, float)):
        raise ValueError("block_ms must be a finite positive number")
    if not math.isfinite(float(block_ms)) or block_ms <= 0:
        raise ValueError("block_ms must be a finite positive number")
    # Mirrors RVCStreamEngine.__init__: zc = max(1, sr // 100), then round
    # the requested block duration to an integer multiple of zc.
    zc = max(1, sample_rate // 100)
    return int(round(float(block_ms) / 1000.0 * sample_rate / zc) * zc)


def build_engine_config(
    rvc_root: str | Path,
    model_path: str | Path,
    index_path: str | Path | None = None,
) -> dict[str, Any]:
    """Create explicit RVC config matching the existing mono 48 kHz IPC ABI."""
    root = Path(rvc_root).expanduser().resolve()
    model = Path(model_path).expanduser().resolve()
    worker_source = root / "RVCRealtimeVST" / "worker" / "rvc_worker.py"
    if not root.is_dir() or not worker_source.is_file():
        raise ValueError("rvc_root must be an RVC checkout containing RVCRealtimeVST/worker/rvc_worker.py")
    if not model.is_file() or model.suffix.lower() != ".pth":
        raise ValueError("model_path must be an existing .pth file")
    if model.stat().st_size == 0:
        raise ValueError("model_path must not be empty")

    config: dict[str, Any] = {
        "rvc_root": str(root),
        "model_path": str(model),
        "index_path": "",
        "sample_rate": HOST_SAMPLE_RATE,
        "block_ms": HOST_BLOCK_MS,
        "crossfade_ms": RVC_CROSSFADE_MS,
        "extra_ms": RVC_EXTRA_MS,
    }
    if index_path is not None:
        index = Path(index_path).expanduser().resolve()
        if not index.is_file() or index.suffix.lower() != ".index":
            raise ValueError("index_path must be an existing .index file")
        if index.stat().st_size == 0:
            raise ValueError("index_path must not be empty")
        config["index_path"] = str(index)

    frames = expected_rvc_block_frames(config["sample_rate"], config["block_ms"])
    if frames != HOST_BLOCK_FRAMES:
        raise StreamContractError(
            f"configured RVC block is {frames} frames; host requires {HOST_BLOCK_FRAMES}"
        )
    return config


class RvcRealtimeAdapter:
    """Validate the stateful upstream engine at the non-real-time Worker edge."""

    def __init__(self, engine: Any):
        self.engine = engine
        sample_rate = getattr(engine, "sample_rate", None)
        block_frame = getattr(engine, "block_frame", None)
        if sample_rate != HOST_SAMPLE_RATE:
            raise StreamContractError(
                f"RVC sample rate {sample_rate!r}; host requires {HOST_SAMPLE_RATE}"
            )
        if block_frame != HOST_BLOCK_FRAMES:
            raise StreamContractError(
                f"RVC block size {block_frame!r}; host requires {HOST_BLOCK_FRAMES}"
            )

    def prewarm(self) -> None:
        warm = getattr(self.engine, "prewarm", None)
        if not callable(warm):
            raise StreamContractError("RVC engine has no callable prewarm()")
        warm()

    def process(
        self,
        audio: AudioBuffer,
        pitch: float,
        formant: float,
        index_rate: float,
        rms_mix: float,
        threshold: float,
        f0_method: int,
    ):
        try:
            input_frames = len(audio)
            input_samples = iter(audio)
        except TypeError as exc:
            raise StreamContractError("input block must be a sized, iterable audio buffer") from exc
        if input_frames != HOST_BLOCK_FRAMES:
            raise StreamContractError(
                f"input block has {input_frames} frames; expected {HOST_BLOCK_FRAMES}"
            )
        input_count = 0
        for index, sample in enumerate(input_samples):
            if index >= input_frames:
                raise StreamContractError(
                    f"input iterator yielded more than {input_frames} samples"
                )
            input_count += 1
            try:
                finite = math.isfinite(float(sample))
            except (TypeError, ValueError, OverflowError):
                finite = False
            if not finite:
                raise StreamContractError(
                    f"input block contains a non-finite sample at index {index}"
                )
        if input_count != input_frames:
            raise StreamContractError(
                f"input iterator yielded {input_count} samples; expected {input_frames}"
            )
        output = self.engine.process(
            audio, pitch, formant, index_rate, rms_mix, threshold, f0_method
        )
        if output is None:
            raise StreamContractError("RVC output is missing; expected 2880 frames")
        try:
            output_frames = len(output)
            output_samples = iter(output)
        except TypeError as exc:
            raise StreamContractError("RVC output must be a sized, iterable audio buffer") from exc
        if output_frames != HOST_BLOCK_FRAMES:
            raise StreamContractError(
                f"RVC output has {output_frames} frames; expected {HOST_BLOCK_FRAMES}"
            )
        output_count = 0
        for index, sample in enumerate(output_samples):
            if index >= output_frames:
                raise StreamContractError(
                    f"RVC output iterator yielded more than {output_frames} samples"
                )
            output_count += 1
            try:
                finite = math.isfinite(float(sample))
            except (TypeError, ValueError, OverflowError):
                finite = False
            if not finite:
                raise StreamContractError(
                    f"RVC output contains a non-finite sample at index {index}"
                )
        if output_count != output_frames:
            raise StreamContractError(
                f"RVC output iterator yielded {output_count} samples; expected {output_frames}"
            )
        return output
