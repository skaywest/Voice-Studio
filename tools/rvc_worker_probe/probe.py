#!/usr/bin/env python3
"""Offline diagnostics for the Voice Studio RVC worker environment."""
from __future__ import annotations

import argparse
import json
import platform
import struct
import sys
from typing import Any


def _release_version(value: Any) -> str:
    """Compare complete package release versions while ignoring local CUDA tags."""
    return str(value or "").split("+", 1)[0].strip()


def _import_torchaudio() -> Any | None:
    try:
        import torchaudio
        return torchaudio
    except Exception:
        return None


def _torch_report(torch_module: Any | None = None,
                  torchaudio_module: Any | None = None) -> dict[str, Any]:
    if torch_module is None:
        try:
            import torch as torch_module
        except Exception as exc:
            return {
                "installed": False,
                "error": f"{type(exc).__name__}: {exc}",
                "cuda_available": False,
                "cuda_smoke_test": False,
                "fp16_smoke_test": False,
            }

    report: dict[str, Any] = {
        "installed": True,
        "version": str(getattr(torch_module, "__version__", "unknown")),
        "cuda_build": getattr(getattr(torch_module, "version", None), "cuda", None),
        "cuda_available": False,
        "cuda_smoke_test": False,
        "fp16_smoke_test": False,
    }

    if torchaudio_module is None:
        torchaudio_module = _import_torchaudio()
    if torchaudio_module is None:
        report["torchaudio"] = {"installed": False}
    else:
        audio_version = str(getattr(torchaudio_module, "__version__", "unknown"))
        torch_version = report["version"]
        torch_release = _release_version(torch_version)
        audio_release = _release_version(audio_version)
        report["torchaudio"] = {
            "installed": True,
            "version": audio_version,
            "version_matches_torch": (
                audio_release == torch_release
                and audio_release not in ("", "unknown")
            ),
        }

    try:
        report["cuda_available"] = bool(torch_module.cuda.is_available())
        if not report["cuda_available"]:
            return report

        index = int(torch_module.cuda.current_device())
        props = torch_module.cuda.get_device_properties(index)
        device = {
            "index": index,
            "name": str(props.name),
            "compute_capability": f"{props.major}.{props.minor}",
            "total_memory_bytes": int(props.total_memory),
        }
        try:
            free_bytes, total_bytes = torch_module.cuda.mem_get_info(index)
            device["free_memory_bytes"] = int(free_bytes)
            device["memory_total_bytes"] = int(total_bytes)
        except Exception as exc:
            device["memory_query_error"] = f"{type(exc).__name__}: {exc}"
        report["device"] = device

        # Exercise an actual CUDA kernel and synchronization, not just device discovery.
        lhs = torch_module.ones((128, 128), device="cuda", dtype=torch_module.float32)
        rhs = torch_module.ones((128, 128), device="cuda", dtype=torch_module.float32)
        result = lhs @ rhs
        torch_module.cuda.synchronize()
        report["cuda_smoke_test"] = bool(torch_module.isfinite(result).all().item())
        report["cuda_smoke_test_value"] = float(result[0, 0].item())

        try:
            half_lhs = torch_module.ones((64, 64), device="cuda", dtype=torch_module.float16)
            half_result = half_lhs @ half_lhs
            torch_module.cuda.synchronize()
            report["fp16_smoke_test"] = bool(torch_module.isfinite(half_result).all().item())
        except Exception as exc:
            report["fp16_error"] = f"{type(exc).__name__}: {exc}"
    except Exception as exc:
        report["cuda_error"] = f"{type(exc).__name__}: {exc}"

    return report


def collect_report(torch_module: Any | None = None,
                   torchaudio_module: Any | None = None) -> dict[str, Any]:
    version = sys.version_info
    torch_info = _torch_report(torch_module, torchaudio_module)
    audio_info = torch_info.get("torchaudio", {})
    return {
        "tool": "Voice Studio RVC environment probe",
        "python": {
            "version": platform.python_version(),
            "major": version.major,
            "minor": version.minor,
            "implementation": platform.python_implementation(),
            "architecture_bits": struct.calcsize("P") * 8,
            "platform": platform.platform(),
            "supported": version.major == 3 and version.minor == 12,
        },
        "pytorch": torch_info,
        "checks": {
            "python_312": version.major == 3 and version.minor == 12,
            "python_x64": struct.calcsize("P") * 8 == 64,
            "pytorch_installed": bool(torch_info["installed"]),
            "torchaudio_installed": bool(audio_info.get("installed", False)),
            "torchaudio_matches_torch": bool(audio_info.get("version_matches_torch", False)),
            "cuda_available": bool(torch_info["cuda_available"]),
            "cuda_128_build": str(torch_info.get("cuda_build") or "").startswith("12.8"),
            "cuda_kernel_passed": bool(torch_info.get("cuda_smoke_test", False)),
            "fp16_kernel_passed": bool(torch_info.get("fp16_smoke_test", False)),
        },
        "network_used": False,
        "model_loaded": False,
        "inference_tested": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument("--require-cuda", action="store_true",
                        help="fail unless Python 3.12 x64, matching torchaudio and CUDA 12.8 GPU tests pass")
    args = parser.parse_args()
    report = collect_report()
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print(report["tool"])
        for section in ("python", "pytorch", "checks"):
            print(f"\n[{section}]")
            for key, value in report[section].items():
                print(f"{key}: {value}")
        print("\nNo network access; no model was loaded; RVC inference was not tested.")
    if args.require_cuda:
        required = (
            "python_312", "python_x64", "pytorch_installed",
            "torchaudio_installed", "torchaudio_matches_torch",
            "cuda_available", "cuda_128_build",
            "cuda_kernel_passed", "fp16_kernel_passed",
        )
        return 0 if all(report["checks"][key] for key in required) else 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
