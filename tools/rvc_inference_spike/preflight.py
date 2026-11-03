#!/usr/bin/env python3
"""Offline preflight for an externally supplied RVC checkout and Python runtime."""
from __future__ import annotations

import argparse
import importlib
import inspect
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
from typing import Any

REQUIRED_FILES = (
    "configs/config.py",
    "infer/rtrvc.py",
    "infer/hubert.py",
    "tools/cuda_graph.py",
)
REQUIRED_MODULES = ("torch", "torchaudio", "numpy", "librosa", "faiss", "parselmouth")


def _runtime_probe(rvc_root: Path) -> dict[str, Any]:
    root = rvc_root.resolve()
    report: dict[str, Any] = {
        "python": {
            "version": platform.python_version(),
            "architecture_bits": struct.calcsize("P") * 8,
            "implementation": platform.python_implementation(),
        },
        "rvc_root": str(root),
        "imports": {},
        "checks": {},
        "network_used": False,
        "model_loaded": False,
        "inference_tested": False,
    }
    os.chdir(root)
    sys.path.insert(0, str(root))
    sys.argv = [sys.argv[0]]

    for name in REQUIRED_MODULES:
        try:
            module = importlib.import_module(name)
            report["imports"][name] = {
                "available": True,
                "version": str(getattr(module, "__version__", "unknown")),
            }
        except Exception as exc:
            report["imports"][name] = {
                "available": False,
                "error": f"{type(exc).__name__}: {exc}",
            }

    torch_info = report["imports"].get("torch", {})
    if torch_info.get("available"):
        torch = importlib.import_module("torch")
        cuda_build = getattr(getattr(torch, "version", None), "cuda", None)
        report["pytorch"] = {
            "version": str(torch.__version__),
            "cuda_build": cuda_build,
            "cuda_available": bool(torch.cuda.is_available()),
        }
        report["checks"]["cuda_128_build"] = str(cuda_build or "").startswith("12.8")
        try:
            if torch.cuda.is_available():
                device_index = int(torch.cuda.current_device())
                props = torch.cuda.get_device_properties(device_index)
                free_bytes, total_bytes = torch.cuda.mem_get_info(device_index)
                x = torch.ones((128, 128), device=f"cuda:{device_index}", dtype=torch.float32)
                y = x @ x
                torch.cuda.synchronize(device_index)
                fp32_ok = bool(torch.isfinite(y).all().item())
                h = torch.ones((64, 64), device=f"cuda:{device_index}", dtype=torch.float16)
                half_result = h @ h
                torch.cuda.synchronize(device_index)
                fp16_ok = bool(torch.isfinite(half_result).all().item())
                report["gpu"] = {
                    "name": str(props.name),
                    "compute_capability": f"{props.major}.{props.minor}",
                    "total_memory_bytes": int(total_bytes),
                    "free_memory_bytes": int(free_bytes),
                    "fp32_kernel_passed": fp32_ok,
                    "fp16_kernel_passed": fp16_ok,
                }
                report["checks"]["cuda_kernel_passed"] = fp32_ok
                report["checks"]["fp16_kernel_passed"] = fp16_ok
            else:
                report["checks"].update({
                    "cuda_kernel_passed": False,
                    "fp16_kernel_passed": False,
                })
        except Exception as exc:
            report["gpu_error"] = f"{type(exc).__name__}: {exc}"
            report["checks"].update({
                "cuda_kernel_passed": False,
                "fp16_kernel_passed": False,
            })

    audio = report["imports"].get("torchaudio", {})
    torch = report.get("pytorch", {})
    if audio.get("available") and torch.get("version"):
        def major_minor(value: str) -> str:
            return ".".join(str(value).split("+", 1)[0].split(".")[:2])
        report["checks"]["torchaudio_matches_torch"] = (
            major_minor(audio["version"]) == major_minor(torch["version"])
        )

    # Import the actual upstream RVC modules only after dependency diagnostics.
    dependencies_ok = all(report["imports"].get(name, {}).get("available") for name in REQUIRED_MODULES)
    if dependencies_ok:
        try:
            config_module = importlib.import_module("configs.config")
            rtrvc = importlib.import_module("infer.rtrvc")
            config = config_module.Config()
            constructor = inspect.signature(rtrvc.RVC)
            infer_method = inspect.signature(rtrvc.RVC.infer)
            report["rvc_api"] = {
                "config_class": hasattr(config_module, "Config"),
                "rvc_class": hasattr(rtrvc, "RVC"),
                "constructor": str(constructor),
                "infer": str(infer_method),
                "device": str(config.device),
                "is_half": bool(config.is_half),
                "target_api_compatible": all(
                    key in constructor.parameters
                    for key in ("key", "formant", "pth_path", "index_path", "index_rate", "config", "last_rvc")
                ) and all(
                    key in infer_method.parameters
                    for key in ("input_wav", "block_frame_16k", "skip_head", "return_length", "f0method")
                ),
            }
            report["checks"]["rvc_api_imported"] = True
            report["checks"]["rvc_api_compatible"] = report["rvc_api"]["target_api_compatible"]
            report["checks"]["rvc_cuda_device_selected"] = str(config.device).startswith("cuda")
        except Exception as exc:
            report["rvc_import_error"] = f"{type(exc).__name__}: {exc}"
            report["checks"]["rvc_api_imported"] = False
            report["checks"]["rvc_api_compatible"] = False
            report["checks"]["rvc_cuda_device_selected"] = False
    else:
        report["checks"].update({
            "rvc_api_imported": False,
            "rvc_api_compatible": False,
            "rvc_cuda_device_selected": False,
        })
    return report


def run_preflight(
    python_exe: Path,
    rvc_root: Path,
    model_path: Path | None = None,
    index_path: Path | None = None,
    timeout_seconds: int = 180,
    runner=subprocess.run,
) -> dict[str, Any]:
    root = rvc_root.expanduser().resolve()
    python_path = python_exe.expanduser().resolve()
    missing = [str(root / item) for item in REQUIRED_FILES if not (root / item).is_file()]
    report: dict[str, Any] = {
        "tool": "Voice Studio RVC inference preflight",
        "python_executable": str(python_path),
        "rvc_root": str(root),
        "required_files": {item: (root / item).is_file() for item in REQUIRED_FILES},
        "missing_files": missing,
        "model": "not_provided",
        "index": "not_provided",
        "network_used": False,
        "model_loaded": False,
        "inference_tested": False,
    }
    if model_path is not None:
        model = model_path.expanduser().resolve()
        report["model"] = {
            "path": str(model),
            "exists": model.is_file(),
            "extension_supported": model.suffix.lower() == ".pth",
            "checkpoint_loaded": False,
        }
    if index_path is not None:
        index = index_path.expanduser().resolve()
        report["index"] = {
            "path": str(index),
            "exists": index.is_file(),
            "extension_supported": index.suffix.lower() == ".index",
        }
    if missing:
        report["runtime_probe"] = {"started": False, "reason": "RVC source tree is incomplete"}
        return report
    if not python_path.is_file():
        report["runtime_probe"] = {"started": False, "reason": "Python executable does not exist"}
        return report

    command = [
        str(python_path), str(Path(__file__).resolve()),
        "--runtime-probe", "--rvc-root", str(root),
    ]
    try:
        completed = runner(
            command,
            cwd=str(root),
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
        report["runtime_probe"] = {
            "started": True,
            "returncode": int(completed.returncode),
            "stdout": completed.stdout[-20000:],
            "stderr": completed.stderr[-10000:],
        }
        if completed.returncode == 0:
            try:
                child = json.loads(completed.stdout)
                report["runtime"] = child
            except json.JSONDecodeError as exc:
                report["runtime_probe"]["json_error"] = str(exc)
        else:
            report["runtime_probe"]["failed"] = True
    except subprocess.TimeoutExpired:
        report["runtime_probe"] = {
            "started": True,
            "timed_out": True,
            "timeout_seconds": timeout_seconds,
        }
    except OSError as exc:
        report["runtime_probe"] = {
            "started": False,
            "reason": f"{type(exc).__name__}: {exc}",
        }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", type=Path, help="External RVC Python executable")
    parser.add_argument("--rvc-root", type=Path, required=True, help="External RVC source checkout")
    parser.add_argument("--model", type=Path, help="Optional user-supplied .pth model; never loaded")
    parser.add_argument("--index", type=Path, help="Optional user-supplied .index file")
    parser.add_argument("--require-cuda", action="store_true")
    parser.add_argument("--runtime-probe", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.runtime_probe:
        print(json.dumps(_runtime_probe(args.rvc_root), ensure_ascii=False))
        return 0

    if not args.python:
        parser.error("--python is required for an external runtime preflight")
    report = run_preflight(args.python, args.rvc_root, args.model, args.index)
    print(json.dumps(report, indent=2, ensure_ascii=False))
    if args.require_cuda:
        runtime = report.get("runtime", {})
        checks = runtime.get("checks", {})
        required = (
            "cuda_128_build", "cuda_kernel_passed", "fp16_kernel_passed",
            "rvc_api_imported", "rvc_api_compatible", "rvc_cuda_device_selected",
        )
        return 0 if all(checks.get(key) is True for key in required) else 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
