#!/usr/bin/env python3
"""Offline diagnostics for the Voice Studio RVC worker environment."""
from __future__ import annotations
import argparse
import json
import platform
import struct
import sys
from typing import Any


def _torch_report(torch_module: Any | None = None) -> dict[str, Any]:
    if torch_module is None:
        try:
            import torch as torch_module
        except Exception as exc:
            return {"installed": False, "error": f"{type(exc).__name__}: {exc}", "cuda_available": False}
    report: dict[str, Any] = {
        "installed": True,
        "version": str(getattr(torch_module, "__version__", "unknown")),
        "cuda_build": getattr(getattr(torch_module, "version", None), "cuda", None),
        "cuda_available": False,
    }
    try:
        report["cuda_available"] = bool(torch_module.cuda.is_available())
        if report["cuda_available"]:
            index = int(torch_module.cuda.current_device())
            props = torch_module.cuda.get_device_properties(index)
            report["device"] = {
                "index": index,
                "name": str(props.name),
                "compute_capability": f"{props.major}.{props.minor}",
                "total_memory_bytes": int(props.total_memory),
            }
    except Exception as exc:
        report["cuda_error"] = f"{type(exc).__name__}: {exc}"
    return report


def collect_report(torch_module: Any | None = None) -> dict[str, Any]:
    version = sys.version_info
    torch_info = _torch_report(torch_module)
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
            "cuda_available": bool(torch_info["cuda_available"]),
            "cuda_128_build": str(torch_info.get("cuda_build") or "").startswith("12.8"),
        },
        "network_used": False,
        "model_loaded": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument("--require-cuda", action="store_true",
                        help="fail unless Python 3.12 x64 and CUDA 12.8 are available")
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
        print("\nNo network access; no model was loaded.")
    if args.require_cuda:
        required = ("python_312", "python_x64", "pytorch_installed", "cuda_available", "cuda_128_build")
        return 0 if all(report["checks"][key] for key in required) else 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
