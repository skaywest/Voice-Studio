"""Static API inspection for the upstream RVC realtime worker.

The source is parsed, never imported or executed. This lets preflight validate
the adapter boundary without importing third-party code or loading a model.
"""
from __future__ import annotations

import ast
from pathlib import Path
from typing import Any

EXPECTED_CONSTRUCTOR = ("self", "cfg")
EXPECTED_PROCESS = (
    "self", "audio", "pitch", "formant", "index_rate",
    "rms_mix", "threshold", "f0_method",
)


def _positional_names(function: ast.FunctionDef | ast.AsyncFunctionDef) -> tuple[str, ...]:
    args = function.args
    return tuple(arg.arg for arg in (*args.posonlyargs, *args.args))


def inspect_realtime_worker_api(worker_path: Path) -> dict[str, Any]:
    path = Path(worker_path)
    report: dict[str, Any] = {
        "path": str(path),
        "available": False,
        "compatible": False,
        "engine_class_found": False,
        "constructor_parameters": [],
        "process_parameters": [],
    }
    try:
        source = path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=str(path))
    except (OSError, UnicodeError, SyntaxError) as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        return report

    engine = next(
        (node for node in tree.body
         if isinstance(node, ast.ClassDef) and node.name == "RVCStreamEngine"),
        None,
    )
    if engine is None:
        report["error"] = "RVCStreamEngine class not found"
        return report

    report["available"] = True
    report["engine_class_found"] = True
    methods = {
        node.name: node
        for node in engine.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    constructor = methods.get("__init__")
    process = methods.get("process")
    if constructor is not None:
        report["constructor_parameters"] = list(_positional_names(constructor))
    if process is not None:
        report["process_parameters"] = list(_positional_names(process))

    report["compatible"] = (
        tuple(report["constructor_parameters"]) == EXPECTED_CONSTRUCTOR
        and tuple(report["process_parameters"]) == EXPECTED_PROCESS
    )
    if not report["compatible"]:
        report["error"] = "RVCStreamEngine constructor/process signature mismatch"
    return report
