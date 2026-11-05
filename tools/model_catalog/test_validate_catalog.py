import copy
import unittest

from validate_catalog import CatalogError, validate_catalog


def valid_entry():
    return {
        "id": "sample.voice01",
        "name": "Sample Voice",
        "description": "A locally processed original sample voice.",
        "category": "male",
        "language": "multi",
        "runtime": {"name": "voice-studio-rvc", "version": "1.0"},
        "files": [{
            "filename": "voice.onnx",
            "size_bytes": 1024,
            "sha256": "a" * 64,
        }],
        "source": {
            "url": "https://example.org/models/sample",
            "publisher": "Example Publisher",
        },
        "license": {
            "id": "CC-BY-4.0",
            "summary": "Attribution required.",
            "redistribution_allowed": True,
        },
        "voice_rights": {
            "status": "original",
            "evidence": "Created specifically for this project.",
        },
        "installable": True,
    }


def valid_catalog():
    return {
        "catalog_version": 1,
        "description": "Test catalog",
        "models": [valid_entry()],
    }


class CatalogValidationTests(unittest.TestCase):
    def test_accepts_empty_default_catalog(self):
        catalog = {
            "catalog_version": 1,
            "description": "Empty catalog by default",
            "models": [],
        }
        self.assertEqual(validate_catalog(catalog)["models"], [])

    def test_accepts_complete_reviewed_model(self):
        self.assertEqual(len(validate_catalog(valid_catalog())["models"]), 1)

    def test_rejects_wrong_root_or_version(self):
        with self.assertRaisesRegex(CatalogError, "root"):
            validate_catalog([])
        catalog = valid_catalog()
        catalog["catalog_version"] = 2
        with self.assertRaisesRegex(CatalogError, "catalog_version"):
            validate_catalog(catalog)

    def test_rejects_duplicate_or_invalid_ids(self):
        catalog = valid_catalog()
        catalog["models"].append(copy.deepcopy(catalog["models"][0]))
        with self.assertRaisesRegex(CatalogError, "duplicate"):
            validate_catalog(catalog)
        catalog = valid_catalog()
        catalog["models"][0]["id"] = "../unsafe"
        with self.assertRaisesRegex(CatalogError, "invalid format"):
            validate_catalog(catalog)

    def test_rejects_path_traversal_and_executable_files(self):
        for filename in ("../voice.onnx", "C:\\temp\\voice.onnx", "loader.exe", "model.py",
                         "voice.onnx.", "voice.onnx ", "CON.txt", "COM1.pth",
                         "name:stream.pth", "голос.pth"):
            catalog = valid_catalog()
            catalog["models"][0]["files"][0]["filename"] = filename
            with self.subTest(filename=filename), self.assertRaises(CatalogError):
                validate_catalog(catalog)

    def test_rejects_invalid_size_and_hash(self):
        for field, value in (("size_bytes", True), ("size_bytes", 0), ("sha256", "xyz")):
            catalog = valid_catalog()
            catalog["models"][0]["files"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(CatalogError):
                validate_catalog(catalog)

    def test_rejects_non_https_source_or_credentials(self):
        for url in ("http://example.org/model", "https://user:pass@example.org/model", "file:///tmp/model"):
            catalog = valid_catalog()
            catalog["models"][0]["source"]["url"] = url
            with self.subTest(url=url), self.assertRaises(CatalogError):
                validate_catalog(catalog)

    def test_blocks_install_without_redistribution_rights(self):
        catalog = valid_catalog()
        catalog["models"][0]["license"]["redistribution_allowed"] = False
        with self.assertRaisesRegex(CatalogError, "redistribution"):
            validate_catalog(catalog)

    def test_blocks_install_without_documented_voice_rights(self):
        catalog = valid_catalog()
        catalog["models"][0]["voice_rights"]["status"] = "unknown"
        with self.assertRaisesRegex(CatalogError, "voice rights"):
            validate_catalog(catalog)

    def test_allows_unverified_entry_only_when_not_installable(self):
        catalog = valid_catalog()
        catalog["models"][0]["installable"] = False
        catalog["models"][0]["voice_rights"]["status"] = "unknown"
        catalog["models"][0]["license"]["redistribution_allowed"] = False
        self.assertEqual(len(validate_catalog(catalog)["models"]), 1)

    def test_rejects_duplicate_filenames_and_unknown_category(self):
        catalog = valid_catalog()
        catalog["models"][0]["files"].append(copy.deepcopy(catalog["models"][0]["files"][0]))
        with self.assertRaisesRegex(CatalogError, "duplicate filenames"):
            validate_catalog(catalog)
        catalog = valid_catalog()
        catalog["models"][0]["files"].append({
            **copy.deepcopy(catalog["models"][0]["files"][0]),
            "filename": "VOICE.ONNX",
        })
        with self.assertRaisesRegex(CatalogError, "case-insensitive"):
            validate_catalog(catalog)
        catalog = valid_catalog()
        catalog["models"][0]["category"] = "not-a-category"
        with self.assertRaisesRegex(CatalogError, "category"):
            validate_catalog(catalog)

    def test_rejects_non_boolean_installable_flag(self):
        catalog = valid_catalog()
        catalog["models"][0]["installable"] = 1
        with self.assertRaisesRegex(CatalogError, "boolean"):
            validate_catalog(catalog)


if __name__ == "__main__":
    unittest.main()
