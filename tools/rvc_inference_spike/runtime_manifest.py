"""Build a bounded, offline Worker runtime manifest for Voice Studio.

This tool inventories an explicitly selected runtime tree and hashes every
regular file. It does not execute code, install packages, or access network.
The native Worker independently validates and hashes every transferred file.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import struct
from pathlib import Path
from typing import Iterable

MAGIC = 0x5653524D  # VSRM
VERSION = 1
MAX_ENTRIES = 512
MAX_PATH_UNITS = 511
MAX_FILE_BYTES = 8 * 1024**3
MAX_TOTAL_BYTES = 32 * 1024**3
DOMAIN = b"VoiceStudio.RuntimeManifest.v1\0"

ROLES = {
    "pythonSource": 1,
    "checkpoint": 2,
    "faissIndex": 3,
    "hubertAsset": 4,
    "f0Asset": 5,
    "configuration": 6,
    "runtimeSupport": 7,
}
DRIVE_PREFIX = re.compile(r"^[A-Za-z]:")


class ManifestError(ValueError):
    pass


def _safe_relative_path(path: str) -> None:
    if not path or len(path.encode("utf-16-le")) // 2 > MAX_PATH_UNITS:
        raise ManifestError(f"invalid or overlong relative path: {path!r}")
    if not path.isascii() or "\\" in path or path.startswith("/") or path.endswith("/"):
        raise ManifestError(f"path is not portable ASCII relative form: {path!r}")
    if DRIVE_PREFIX.match(path):
        raise ManifestError(f"drive-qualified path is forbidden: {path!r}")
    for component in path.split("/"):
        if component in ("", ".", "..") or component.endswith((".", " ")):
            raise ManifestError(f"unsafe path component: {component!r}")
        base = component.split(".", 1)[0].upper()
        if base in {"CON", "PRN", "AUX", "NUL"} or re.fullmatch(r"(COM|LPT)[1-9]", base):
            raise ManifestError(f"reserved Windows path component: {component!r}")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", component):
            raise ManifestError(f"unsupported path component: {component!r}")


def _role_for(path: str) -> str:
    lower = path.lower()
    name = lower.rsplit("/", 1)[-1]
    if lower.endswith(".pth"):
        return "checkpoint"
    if lower.endswith(".index"):
        return "faissIndex"
    if "hubert" in lower or "/hubert" in lower:
        return "hubertAsset"
    if "rmvpe" in lower or "/f0/" in lower or "f0" in name:
        return "f0Asset"
    if lower.endswith(".py"):
        return "pythonSource"
    if lower.endswith((".json", ".yaml", ".yml", ".ini", ".toml", ".txt")):
        return "configuration"
    return "runtimeSupport"


def _validate_unique_paths(paths: list[str]) -> None:
    """Reject case-insensitive duplicates and file/directory prefix collisions."""
    normalized = sorted(path.lower() for path in paths)
    for index, path in enumerate(normalized):
        if index and path == normalized[index - 1]:
            raise ManifestError(f"case-insensitive duplicate runtime path: {path!r}")
        for other in normalized[index + 1:]:
            if not other.startswith(path):
                break
            if other[len(path):].startswith("/"):
                raise ManifestError(
                    f"runtime file/directory path collision: {path!r} and {other!r}"
                )


def _hash_file(path: Path) -> tuple[int, str]:
    before = path.stat(follow_symlinks=False)
    if not stat.S_ISREG(before.st_mode):
        raise ManifestError(f"not a regular file: {path}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino, opened.st_size) != (
            before.st_dev, before.st_ino, before.st_size
        ):
            raise ManifestError(f"file changed while opening: {path}")
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            if size > MAX_FILE_BYTES:
                raise ManifestError(f"file exceeds 8 GiB limit: {path}")
            digest.update(chunk)
        after = os.fstat(stream.fileno())
        if (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) != (
            opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns
        ) or size != opened.st_size:
            raise ManifestError(f"file changed while hashing: {path}")
    return size, digest.hexdigest()


def _canonical_digest(entries: list[dict], total_bytes: int) -> str:
    data = bytearray(DOMAIN)
    data += struct.pack("<IHIQ", MAGIC, VERSION, len(entries), total_bytes)
    for entry in entries:
        encoded = entry["path"].encode("utf-16-le")
        data += struct.pack("<HHQ", ROLES[entry["role"]], len(encoded) // 2, entry["size_bytes"])
        data += bytes.fromhex(entry["sha256"])
        data += encoded
    return hashlib.sha256(data).hexdigest()


def build_manifest(root: str | Path) -> dict:
    base = Path(root).expanduser().resolve(strict=True)
    if not base.is_dir():
        raise ManifestError("runtime root must be a directory")

    found: list[tuple[str, Path]] = []
    for current, dirs, files in os.walk(base, followlinks=False):
        current_path = Path(current)
        for name in list(dirs):
            candidate = current_path / name
            if candidate.is_symlink():
                raise ManifestError(f"symbolic-link directory is forbidden: {candidate}")
        dirs.sort(key=str.lower)
        for name in files:
            candidate = current_path / name
            if candidate.is_symlink():
                raise ManifestError(f"symbolic-link file is forbidden: {candidate}")
            relative = candidate.relative_to(base).as_posix()
            _safe_relative_path(relative)
            found.append((relative, candidate))

    found.sort(key=lambda item: (item[0].lower(), item[0]))
    _validate_unique_paths([relative for relative, _ in found])
    if not found:
        raise ManifestError("runtime root contains no files")
    if len(found) > MAX_ENTRIES:
        raise ManifestError(f"runtime contains {len(found)} files; native ABI limit is {MAX_ENTRIES}")

    entries = []
    total = 0
    for relative, path in found:
        size, digest = _hash_file(path)
        if size == 0:
            raise ManifestError(f"empty runtime file is not allowed: {relative}")
        total += size
        if total > MAX_TOTAL_BYTES:
            raise ManifestError("runtime exceeds 32 GiB total transfer limit")
        entries.append({
            "path": relative,
            "role": _role_for(relative),
            "size_bytes": size,
            "sha256": digest,
        })

    return {
        "format": "voice-studio-runtime-manifest",
        "version": VERSION,
        "entry_count": len(entries),
        "total_bytes": total,
        "sha256": _canonical_digest(entries, total),
        "entries": entries,
    }


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Hash an offline Voice Studio runtime tree")
    parser.add_argument("--root", required=True, help="explicit runtime/source directory")
    parser.add_argument("--output", required=True, help="JSON manifest output path")
    args = parser.parse_args(argv)
    try:
        manifest = build_manifest(args.root)
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (OSError, ManifestError) as exc:
        parser.error(str(exc))
    print(f"Manifest ready: {manifest['entry_count']} files, {manifest['total_bytes']} bytes")
    print(f"SHA-256: {manifest['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
