"""Explicit, offline RVC/model selection manifest.

This module validates user-selected files and fingerprints them. It never
imports RVC code, deserializes a checkpoint, starts a process, or accesses
the network. The trust confirmations are deliberate gates for later loading.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, BinaryIO

MANIFEST_VERSION = 1
RVC_ENTRYPOINT = Path("RVCRealtimeVST/worker/rvc_worker.py")
HASH_CHUNK_BYTES = 1024 * 1024


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(HASH_CHUNK_BYTES)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _selected_file(value: str | Path, extension: str, label: str) -> Path:
    path = Path(value).expanduser().resolve(strict=True)
    if not path.is_file() or path.suffix.lower() != extension:
        raise ValueError(f"{label} must be an existing {extension} file")
    if path.stat().st_size <= 0:
        raise ValueError(f"{label} must not be empty")
    return path


def build_selection_manifest(
    rvc_root: str | Path,
    model_path: str | Path,
    *,
    index_path: str | Path | None = None,
    trust_rvc_source: bool = False,
    confirm_voice_rights: bool = False,
) -> dict[str, Any]:
    """Validate explicit selections and return a content-fingerprinted manifest.

    RVC source is executable Python and checkpoints may contain pickle data.
    Both confirmations must be explicit before this manifest can be produced.
    No source or checkpoint is executed/deserialized here.
    """
    if trust_rvc_source is not True:
        raise ValueError("explicit trust confirmation for the selected RVC source is required")
    if confirm_voice_rights is not True:
        raise ValueError("confirmation of model license and voice rights is required")

    root = Path(rvc_root).expanduser().resolve(strict=True)
    if not root.is_dir():
        raise ValueError("rvc_root must be an existing directory")
    entrypoint = (root / RVC_ENTRYPOINT).resolve(strict=True)
    if not entrypoint.is_file() or not entrypoint.is_relative_to(root):
        raise ValueError("RVC worker entrypoint must be a file inside the selected RVC directory")

    model = _selected_file(model_path, ".pth", "model_path")
    manifest: dict[str, Any] = {
        "manifest_version": MANIFEST_VERSION,
        "rvc": {
            "root": str(root),
            "entrypoint": str(entrypoint),
            "entrypoint_size_bytes": entrypoint.stat().st_size,
            "entrypoint_sha256": _sha256_file(entrypoint),
            "source_trusted_by_user": True,
        },
        "model": {
            "path": str(model),
            "size_bytes": model.stat().st_size,
            "sha256": _sha256_file(model),
            "voice_rights_confirmed_by_user": True,
            "checkpoint_deserialized": False,
        },
        "network_used": False,
        "code_executed": False,
        "inference_tested": False,
    }

    if index_path is not None:
        index = _selected_file(index_path, ".index", "index_path")
        manifest["index"] = {
            "path": str(index),
            "size_bytes": index.stat().st_size,
            "sha256": _sha256_file(index),
        }
    else:
        manifest["index"] = None

    return manifest


def verify_selection_manifest(manifest: dict[str, Any]) -> bool:
    """Re-check selected file identities before a future Worker launch."""
    if not isinstance(manifest, dict) or manifest.get("manifest_version") != MANIFEST_VERSION:
        return False
    try:
        rvc = manifest["rvc"]
        model = manifest["model"]
        root = Path(rvc["root"]).resolve(strict=True)
        entrypoint = Path(rvc["entrypoint"]).resolve(strict=True)
        selected_model = Path(model["path"]).resolve(strict=True)
        if not root.is_dir() or not entrypoint.is_file() or not entrypoint.is_relative_to(root):
            return False
        if not selected_model.is_file() or selected_model.suffix.lower() != ".pth":
            return False
        if entrypoint.stat().st_size != rvc["entrypoint_size_bytes"]:
            return False
        if selected_model.stat().st_size != model["size_bytes"]:
            return False
        if _sha256_file(entrypoint) != rvc["entrypoint_sha256"]:
            return False
        if _sha256_file(selected_model) != model["sha256"]:
            return False
        index = manifest.get("index")
        if index is not None:
            selected_index = Path(index["path"]).resolve(strict=True)
            if not selected_index.is_file() or selected_index.suffix.lower() != ".index":
                return False
            if selected_index.stat().st_size != index["size_bytes"]:
                return False
            if _sha256_file(selected_index) != index["sha256"]:
                return False
        return (
            rvc.get("source_trusted_by_user") is True
            and model.get("voice_rights_confirmed_by_user") is True
            and model.get("checkpoint_deserialized") is False
            and manifest.get("network_used") is False
            and manifest.get("code_executed") is False
            and manifest.get("inference_tested") is False
        )
    except (KeyError, OSError, TypeError, ValueError):
        return False
