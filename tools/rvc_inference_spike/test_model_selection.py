import tempfile
import unittest
from pathlib import Path

from model_selection import build_selection_manifest, verify_selection_manifest


class ModelSelectionTests(unittest.TestCase):
    def make_selection(self, temp):
        root = Path(temp) / "rvc"
        entrypoint = root / "RVCRealtimeVST" / "worker" / "rvc_worker.py"
        entrypoint.parent.mkdir(parents=True)
        entrypoint.write_text("# trusted fixture\\n", encoding="utf-8")
        model = Path(temp) / "voice.pth"
        model.write_bytes(b"checkpoint fixture")
        index = Path(temp) / "voice.index"
        index.write_bytes(b"index fixture")
        return root, entrypoint, model, index

    def test_requires_explicit_source_trust_and_voice_rights(self):
        with tempfile.TemporaryDirectory() as temp:
            root, _, model, _ = self.make_selection(temp)
            with self.assertRaisesRegex(ValueError, "trust confirmation"):
                build_selection_manifest(root, model, confirm_voice_rights=True)
            with self.assertRaisesRegex(ValueError, "voice rights"):
                build_selection_manifest(root, model, trust_rvc_source=True)

    def test_builds_offline_manifest_without_deserializing_checkpoint(self):
        with tempfile.TemporaryDirectory() as temp:
            root, entrypoint, model, index = self.make_selection(temp)
            manifest = build_selection_manifest(
                root, model, index_path=index,
                trust_rvc_source=True, confirm_voice_rights=True,
            )
            self.assertTrue(verify_selection_manifest(manifest))
            self.assertEqual(manifest["model"]["size_bytes"], len(b"checkpoint fixture"))
            self.assertEqual(len(manifest["model"]["sha256"]), 64)
            self.assertEqual(len(manifest["rvc"]["entrypoint_sha256"]), 64)
            self.assertFalse(manifest["model"]["checkpoint_deserialized"])
            self.assertFalse(manifest["code_executed"])
            self.assertFalse(manifest["network_used"])
            self.assertFalse(manifest["inference_tested"])

    def test_rejects_modified_model_or_rvc_entrypoint(self):
        with tempfile.TemporaryDirectory() as temp:
            root, entrypoint, model, _ = self.make_selection(temp)
            manifest = build_selection_manifest(
                root, model, trust_rvc_source=True, confirm_voice_rights=True,
            )
            model.write_bytes(b"changed checkpoint")
            self.assertFalse(verify_selection_manifest(manifest))
            model.write_bytes(b"checkpoint fixture")
            entrypoint.write_text("# modified source\\n", encoding="utf-8")
            self.assertFalse(verify_selection_manifest(manifest))

    def test_rejects_entrypoint_symlink_escaping_selected_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root, entrypoint, model, _ = self.make_selection(temp)
            outside = Path(temp) / "outside.py"
            outside.write_text("# outside\\n", encoding="utf-8")
            entrypoint.unlink()
            try:
                entrypoint.symlink_to(outside)
            except (OSError, NotImplementedError):
                self.skipTest("symlink creation is unavailable")
            with self.assertRaisesRegex(ValueError, "inside the selected RVC directory"):
                build_selection_manifest(
                    root, model, trust_rvc_source=True, confirm_voice_rights=True,
                )

    def test_rejects_empty_or_wrong_extension_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root, _, model, index = self.make_selection(temp)
            model.write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "must not be empty"):
                build_selection_manifest(
                    root, model, trust_rvc_source=True, confirm_voice_rights=True,
                )
            model.write_bytes(b"fixture")
            index.write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "must not be empty"):
                build_selection_manifest(
                    root, model, index_path=index,
                    trust_rvc_source=True, confirm_voice_rights=True,
                )

    def test_verifier_rejects_manifest_tampering(self):
        with tempfile.TemporaryDirectory() as temp:
            root, _, model, _ = self.make_selection(temp)
            manifest = build_selection_manifest(
                root, model, trust_rvc_source=True, confirm_voice_rights=True,
            )
            manifest["model"]["voice_rights_confirmed_by_user"] = False
            self.assertFalse(verify_selection_manifest(manifest))


if __name__ == "__main__":
    unittest.main()
