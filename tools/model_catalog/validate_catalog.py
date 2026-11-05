#!/usr/bin/env python3
"""Validate the offline Voice Studio model catalog without loading model files."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

CATALOG_VERSION = 1
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MODEL_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{2,63}$")
ALLOWED_CATEGORIES = {
    "male", "female", "deep", "bright", "robotic", "demon",
    "monster", "character", "stylized", "radio", "other",
}
FORBIDDEN_EXTENSIONS = {
    ".exe", ".dll", ".sys", ".msi", ".bat", ".cmd", ".ps1",
    ".py", ".pyc", ".com", ".scr", ".vbs", ".js", ".jar",
}
REQUIRED_MODEL_FIELDS = {
    "id", "name", "description", "category", "language", "runtime",
    "files", "source", "license", "voice_rights", "installable",
}


class CatalogError(ValueError):
    """Raised when catalog metadata violates the model-library contract."""


def _require_text(value: Any, field: str, *, maximum: int = 2048) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise CatalogError(f"{field} must be a non-empty string of at most {maximum} characters")
    if any(ord(ch) < 32 and ch not in "\t\n\r" for ch in value):
        raise CatalogError(f"{field} contains control characters")
    return value.strip()


def _require_https_url(value: Any, field: str) -> str:
    url = _require_text(value, field, maximum=2048)
    parsed = urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise CatalogError(f"{field} must be a canonical HTTPS URL without embedded credentials")
    if parsed.fragment:
        raise CatalogError(f"{field} must not contain a URL fragment")
    return url


def validate_catalog(data: Any) -> dict[str, Any]:
    """Validate catalog metadata; never opens, imports, or deserializes model files."""
    if not isinstance(data, dict):
        raise CatalogError("catalog root must be a JSON object")
    if data.get("catalog_version") != CATALOG_VERSION:
        raise CatalogError(f"catalog_version must equal {CATALOG_VERSION}")
    _require_text(data.get("description"), "description", maximum=1000)
    models = data.get("models")
    if not isinstance(models, list):
        raise CatalogError("models must be an array")

    seen_ids: set[str] = set()
    for index, model in enumerate(models):
        prefix = f"models[{index}]"
        if not isinstance(model, dict):
            raise CatalogError(f"{prefix} must be an object")
        missing = REQUIRED_MODEL_FIELDS - model.keys()
        if missing:
            raise CatalogError(f"{prefix} is missing required fields: {', '.join(sorted(missing))}")

        model_id = _require_text(model["id"], f"{prefix}.id", maximum=64)
        if not MODEL_ID_RE.fullmatch(model_id):
            raise CatalogError(f"{prefix}.id has an invalid format")
        if model_id in seen_ids:
            raise CatalogError(f"duplicate model id: {model_id}")
        seen_ids.add(model_id)

        _require_text(model["name"], f"{prefix}.name", maximum=100)
        _require_text(model["description"], f"{prefix}.description", maximum=1000)
        category = _require_text(model["category"], f"{prefix}.category", maximum=32).lower()
        if category not in ALLOWED_CATEGORIES:
            raise CatalogError(f"{prefix}.category is not supported")
        language = _require_text(model["language"], f"{prefix}.language", maximum=32)
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,31}", language):
            raise CatalogError(f"{prefix}.language has an invalid format")

        runtime = model["runtime"]
        if not isinstance(runtime, dict):
            raise CatalogError(f"{prefix}.runtime must be an object")
        _require_text(runtime.get("name"), f"{prefix}.runtime.name", maximum=64)
        _require_text(runtime.get("version"), f"{prefix}.runtime.version", maximum=64)

        files = model["files"]
        if not isinstance(files, list) or not files:
            raise CatalogError(f"{prefix}.files must be a non-empty array")
        seen_filenames: set[str] = set()
        for file_index, item in enumerate(files):
            file_prefix = f"{prefix}.files[{file_index}]"
            if not isinstance(item, dict):
                raise CatalogError(f"{file_prefix} must be an object")
            raw_filename = item.get("filename")
            if isinstance(raw_filename, str) and raw_filename != raw_filename.strip():
                raise CatalogError(f"{file_prefix}.filename must not contain leading or trailing whitespace")
            filename = _require_text(raw_filename, f"{file_prefix}.filename", maximum=128)
            if (filename in {".", ".."} or Path(filename).name != filename
                    or "/" in filename or "\\" in filename
                    or filename.endswith((".", " "))
                    or not filename.isascii()
                    or not re.fullmatch(r"[A-Za-z0-9_.-]+", filename)):
                raise CatalogError(f"{file_prefix}.filename must be a portable plain filename")
            device_name = filename.split(".", 1)[0].upper()
            if (device_name in {"CON", "PRN", "AUX", "NUL"}
                    or re.fullmatch(r"(COM|LPT)([1-9]|[¹²³])", device_name)):
                raise CatalogError(f"{file_prefix}.filename uses a reserved Windows device name")
            if Path(filename).suffix.lower() in FORBIDDEN_EXTENSIONS:
                raise CatalogError(f"{file_prefix}.filename uses a forbidden executable/script extension")
            filename_key = filename.casefold()
            if filename_key in seen_filenames:
                raise CatalogError(f"{prefix} contains duplicate filenames (case-insensitive)")
            seen_filenames.add(filename_key)

            size = item.get("size_bytes")
            if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
                raise CatalogError(f"{file_prefix}.size_bytes must be a positive integer")
            digest = _require_text(item.get("sha256"), f"{file_prefix}.sha256", maximum=64).lower()
            if not SHA256_RE.fullmatch(digest):
                raise CatalogError(f"{file_prefix}.sha256 must be a 64-character SHA-256 hex digest")

        source = model["source"]
        if not isinstance(source, dict):
            raise CatalogError(f"{prefix}.source must be an object")
        _require_https_url(source.get("url"), f"{prefix}.source.url")
        _require_text(source.get("publisher"), f"{prefix}.source.publisher", maximum=200)

        license_info = model["license"]
        if not isinstance(license_info, dict):
            raise CatalogError(f"{prefix}.license must be an object")
        _require_text(license_info.get("id"), f"{prefix}.license.id", maximum=100)
        _require_text(license_info.get("summary"), f"{prefix}.license.summary", maximum=1000)
        if license_info.get("redistribution_allowed") is not True:
            if model["installable"] is True:
                raise CatalogError(f"{prefix} cannot be installable without redistribution permission")

        rights = model["voice_rights"]
        if not isinstance(rights, dict):
            raise CatalogError(f"{prefix}.voice_rights must be an object")
        rights_status = _require_text(rights.get("status"), f"{prefix}.voice_rights.status", maximum=32)
        if rights_status not in {"original", "public_domain", "documented_permission"}:
            if model["installable"] is True:
                raise CatalogError(f"{prefix} cannot be installable without verified voice rights")
        _require_text(rights.get("evidence"), f"{prefix}.voice_rights.evidence", maximum=1000)

        if not isinstance(model["installable"], bool):
            raise CatalogError(f"{prefix}.installable must be a boolean")
        if model["installable"] is True:
            if license_info.get("redistribution_allowed") is not True:
                raise CatalogError(f"{prefix} installable model must allow redistribution")
            if rights_status not in {"original", "public_domain", "documented_permission"}:
                raise CatalogError(f"{prefix} installable model must have verified voice rights")

    return data


def validate_catalog_file(path: str | Path) -> dict[str, Any]:
    catalog_path = Path(path)
    try:
        data = json.loads(catalog_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CatalogError(f"cannot read valid UTF-8 JSON catalog: {exc}") from exc
    return validate_catalog(data)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", nargs="?", default="Models/catalog.json")
    args = parser.parse_args(argv)
    try:
        catalog = validate_catalog_file(args.catalog)
    except CatalogError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 2
    print(f"PASS: catalog schema valid ({len(catalog['models'])} model entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
