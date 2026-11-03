"""Static API inspection for the upstream RVC realtime worker.

The source is parsed, never imported or executed. This validates only the
Python call boundary; it does not prove sample-rate, block-size, latency, or
audio-output compatibility.
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


def _method_report(function: ast.FunctionDef | ast.AsyncFunctionDef | None) -> dict[str, Any]:
    if function is None:
        return {
            "present": False,
            "async": False,
            "positional_parameters": [],
            "keyword_only_parameters": [],
            "accepts_varargs": False,
            "accepts_kwargs": False,
            "descriptor": "missing",
        }

    args = function.args
    decorators = {
        item.id if isinstance(item, ast.Name)
        else item.attr if isinstance(item, ast.Attribute)
        else ""
        for item in function.decorator_list
    }
    descriptor = (
        "staticmethod" if "staticmethod" in decorators
        else "classmethod" if "classmethod" in decorators
        else "instance"
    )
    return {
        "present": True,
        "async": isinstance(function, ast.AsyncFunctionDef),
        "positional_parameters": [
            arg.arg for arg in (*args.posonlyargs, *args.args)
        ],
        "keyword_only_parameters": [arg.arg for arg in args.kwonlyargs],
        "accepts_varargs": args.vararg is not None,
        "accepts_kwargs": args.kwarg is not None,
        "descriptor": descriptor,
    }


def _is_exact_instance_method(report: dict[str, Any], expected: tuple[str, ...]) -> bool:
    return (
        report["present"]
        and not report["async"]
        and tuple(report["positional_parameters"]) == expected
        and not report["keyword_only_parameters"]
        and not report["accepts_varargs"]
        and not report["accepts_kwargs"]
        and report["descriptor"] == "instance"
    )


def inspect_realtime_worker_api(worker_path: Path) -> dict[str, Any]:
    path = Path(worker_path)
    report: dict[str, Any] = {
        "path": str(path),
        "available": False,
        "compatible": False,
        "engine_class_found": False,
        "constructor": _method_report(None),
        "process": _method_report(None),
        # The upstream Python signature does not declare a fixed audio block
        # contract. This remains false until a real adapter/runtime probe
        # verifies sample rate, block framing, output length and statefulness.
        "block_contract_verified": False,
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
    report["constructor"] = _method_report(methods.get("__init__"))
    report["process"] = _method_report(methods.get("process"))
    report["compatible"] = (
        _is_exact_instance_method(report["constructor"], EXPECTED_CONSTRUCTOR)
        and _is_exact_instance_method(report["process"], EXPECTED_PROCESS)
    )
    if not report["compatible"]:
        report["error"] = "RVCStreamEngine constructor/process call contract mismatch"
    return report
