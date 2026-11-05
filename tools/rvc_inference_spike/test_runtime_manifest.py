import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from runtime_manifest import (ManifestError, build_manifest, _canonical_digest,\n                               _safe_relative_path, _validate_unique_paths)


class RuntimeManifestTests(unittest.TestCase):
    def make_tree(self, root):
        (root / "RVCRealtimeVST" / "worker").mkdir(parents=True)
        (root / "RVCRealtimeVST" / "worker" / "rvc_worker.py").write_text("pass\n", encoding="utf-8")
        (root / "infer").mkdir()
        (root / "infer" / "rtrvc.py").write_text("class RVC: pass\n", encoding="utf-8")
        (root / "assets" / "hubert_base").mkdir(parents=True)
        (root / "assets" / "hubert_base" / "config.json").write_text("{}", encoding="utf-8")
        (root / "voice.pth").write_bytes(b"checkpoint")
        (root / "voice.index").write_bytes(b"index")

    def test_manifest_hashes_complete_tree_and_assigns_roles(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "runtime"
            root.mkdir()
            self.make_tree(root)
            manifest = build_manifest(root)
        self.assertEqual(manifest["entry_count"], 5)
        roles = {entry["path"]: entry["role"] for entry in manifest["entries"]}
        self.assertEqual(roles["voice.pth"], "checkpoint")
        self.assertEqual(roles["voice.index"], "faissIndex")
        self.assertEqual(roles["assets/hubert_base/config.json"], "hubertAsset")
        self.assertEqual(roles["infer/rtrvc.py"], "pythonSource")
        self.assertEqual(manifest["sha256"], _canonical_digest(manifest["entries"], manifest["total_bytes"]))

    def test_manifest_is_deterministic(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_tree(root)
            first = build_manifest(root)
            second = build_manifest(root)
        self.assertEqual(first, second)

    def test_content_change_changes_digest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_tree(root)
            first = build_manifest(root)
            (root / "infer" / "rtrvc.py").write_text("changed\n", encoding="utf-8")
            second = build_manifest(root)
        self.assertNotEqual(first["sha256"], second["sha256"])

    def test_rejects_empty_files_and_empty_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(ManifestError):
                build_manifest(root)
            (root / "empty.bin").touch()
            with self.assertRaisesRegex(ManifestError, "empty runtime file"):
                build_manifest(root)

    def test_rejects_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "target.py"
            target.write_text("pass")
            link = root / "link.py"
            try:
                link.symlink_to(target)
            except (OSError, NotImplementedError):
                self.skipTest("symlinks unavailable")
            with self.assertRaisesRegex(ManifestError, "symbolic-link"):
                build_manifest(root)

    def test_rejects_windows_path_aliases(self):
        for path in ("CON.txt", "nul", "COM1.log", "LPT9.data"):
            with self.subTest(path=path):
                with self.assertRaisesRegex(ManifestError, "reserved Windows"):
                    _safe_relative_path(path)

    def test_rejects_case_insensitive_duplicate_paths(self):
        with self.assertRaisesRegex(ManifestError, "case-insensitive duplicate"):
            _validate_unique_paths(["Voice.pth", "voice.PTH"])

    def test_rejects_file_directory_path_collisions(self):
        with self.assertRaisesRegex(ManifestError, "file/directory path collision"):
            _validate_unique_paths(["models", "models/voice.pth"])

    def test_allows_distinct_sibling_paths(self):
        _validate_unique_paths(["models/a.pth", "models/b.pth", "models2/c.pth"])

    def test_rejects_non_ascii_path(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "голос.bin").write_text("x")
            with self.assertRaisesRegex(ManifestError, "portable ASCII"):
                build_manifest(root)


if __name__ == "__main__":
    unittest.main()
