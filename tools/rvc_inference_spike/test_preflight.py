import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace

from preflight import REQUIRED_FILES, run_preflight, strict_cuda_requirements_met


class FakeRunner:
    def __init__(self, returncode=0, stdout=None, stderr=""):
        self.returncode = returncode
        self.stdout = stdout if stdout is not None else json.dumps({"checks": {"cuda_kernel_passed": True}})
        self.stderr = stderr
        self.calls = []

    def __call__(self, command, **kwargs):
        self.calls.append((command, kwargs))
        return SimpleNamespace(returncode=self.returncode, stdout=self.stdout, stderr=self.stderr)


class PreflightTests(unittest.TestCase):
    def make_tree(self, root):
        for relative in REQUIRED_FILES:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# test fixture\n", encoding="utf-8")

    def test_strict_cuda_gate_requires_python_312_x64(self):
        checks = {
            "cuda_128_build": True,
            "cuda_kernel_passed": True,
            "fp16_kernel_passed": True,
            "rvc_api_imported": True,
            "rvc_api_compatible": True,
            "rvc_cuda_device_selected": True,
        }
        self.assertFalse(strict_cuda_requirements_met(checks))
        checks["python_312"] = True
        self.assertFalse(strict_cuda_requirements_met(checks))
        checks["python_64bit"] = True
        self.assertTrue(strict_cuda_requirements_met(checks))
        checks["python_312"] = False
        self.assertFalse(strict_cuda_requirements_met(checks))

    def test_missing_rvc_files_prevents_subprocess(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            runner = FakeRunner()
            report = run_preflight(Path(temp) / "python.exe", root, runner=runner)
            self.assertTrue(report["missing_files"])
            self.assertFalse(report["runtime_probe"]["started"])
            self.assertEqual(runner.calls, [])

    def test_missing_config_profile_prevents_subprocess(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            self.make_tree(root)
            (root / "configs/v2/48k.json").unlink()
            python = Path(temp) / "python.exe"
            python.write_bytes(b"fake")
            runner = FakeRunner()
            report = run_preflight(python, root, runner=runner)
            self.assertIn(str((root / "configs/v2/48k.json").resolve()), report["missing_files"])
            self.assertFalse(report["runtime_probe"]["started"])
            self.assertEqual(runner.calls, [])

    def test_missing_python_prevents_subprocess(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            self.make_tree(root)
            runner = FakeRunner()
            report = run_preflight(Path(temp) / "missing-python.exe", root, runner=runner)
            self.assertFalse(report["runtime_probe"]["started"])
            self.assertIn("does not exist", report["runtime_probe"]["reason"])

    def test_valid_runtime_probe_json_is_parsed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            self.make_tree(root)
            python = Path(temp) / "python.exe"
            python.write_bytes(b"fake")
            payload = {"checks": {"cuda_kernel_passed": True, "rvc_api_compatible": True}}
            runner = FakeRunner(stdout=json.dumps(payload))
            report = run_preflight(python, root, runner=runner)
            self.assertEqual(report["runtime"], payload)
            self.assertEqual(len(runner.calls), 1)
            self.assertEqual(runner.calls[0][1]["timeout"], 180)

    def test_nonzero_runtime_probe_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            self.make_tree(root)
            python = Path(temp) / "python.exe"
            python.write_bytes(b"fake")
            runner = FakeRunner(returncode=1, stderr="import failed")
            report = run_preflight(python, root, runner=runner)
            self.assertTrue(report["runtime_probe"]["failed"])
            self.assertEqual(report["runtime_probe"]["stderr"], "import failed")

    def test_timeout_is_reported(self):
        from subprocess import TimeoutExpired
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            self.make_tree(root)
            python = Path(temp) / "python.exe"
            python.write_bytes(b"fake")
            def timeout_runner(*args, **kwargs):
                raise TimeoutExpired(args[0], kwargs["timeout"])
            report = run_preflight(python, root, runner=timeout_runner)
            self.assertTrue(report["runtime_probe"]["timed_out"])

    def test_model_and_index_are_only_checked_not_loaded(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "rvc"
            root.mkdir()
            self.make_tree(root)
            python = Path(temp) / "python.exe"
            python.write_bytes(b"fake")
            model = Path(temp) / "voice.pth"
            model.write_bytes(b"placeholder")
            index = Path(temp) / "voice.index"
            index.write_bytes(b"placeholder")
            report = run_preflight(python, root, model, index, runner=FakeRunner())
            self.assertTrue(report["model"]["exists"])
            self.assertTrue(report["model"]["extension_supported"])
            self.assertFalse(report["model"]["checkpoint_loaded"])
            self.assertTrue(report["index"]["exists"])
            self.assertFalse(report["model_loaded"])
            self.assertFalse(report["inference_tested"])


if __name__ == "__main__":
    unittest.main()
