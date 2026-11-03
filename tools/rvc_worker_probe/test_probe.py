import unittest
from unittest.mock import patch

from probe import collect_report, _torch_report


class FakeProps:
    name = "NVIDIA GeForce RTX 5060 Ti"
    major = 12
    minor = 0
    total_memory = 16 * 1024**3


class FakeCuda:
    @staticmethod
    def is_available():
        return True

    @staticmethod
    def current_device():
        return 0

    @staticmethod
    def get_device_properties(index):
        assert index == 0
        return FakeProps()

    @staticmethod
    def mem_get_info(index):
        assert index == 0
        return 8 * 1024**3, 16 * 1024**3

    @staticmethod
    def synchronize():
        return None


class FakeTensor:
    def __init__(self, value=16384.0):
        self.value = value

    def __matmul__(self, other):
        return FakeTensor(self.value)

    def all(self):
        return self

    def item(self):
        return True if self.value else False

    def __getitem__(self, index):
        return self


class FakeTorch:
    __version__ = "2.7.1+cu128"
    version = type("Version", (), {"cuda": "12.8"})
    cuda = FakeCuda()
    float32 = "float32"
    float16 = "float16"

    @staticmethod
    def ones(shape, device, dtype):
        assert device == "cuda"
        return FakeTensor()

    @staticmethod
    def isfinite(tensor):
        return tensor


class FakeTorchCudaFailure(FakeTorch):
    class cuda(FakeCuda):
        @staticmethod
        def synchronize():
            raise RuntimeError("simulated CUDA kernel failure")


class FakeTorchCpu(FakeTorch):
    class cuda(FakeCuda):
        @staticmethod
        def is_available():
            return False


class FakeTorchaudio:
    __version__ = "2.7.1+cu128"


class ProbeTests(unittest.TestCase):
    def test_cuda_kernel_and_fp16_are_exercised(self):
        report = collect_report(FakeTorch, FakeTorchaudio)
        self.assertTrue(report["pytorch"]["cuda_available"])
        self.assertTrue(report["pytorch"]["cuda_smoke_test"])
        self.assertTrue(report["pytorch"]["fp16_smoke_test"])
        self.assertEqual(report["pytorch"]["device"]["name"], "NVIDIA GeForce RTX 5060 Ti")
        self.assertEqual(report["pytorch"]["device"]["free_memory_bytes"], 8 * 1024**3)

    def test_torchaudio_version_mismatch_is_reported(self):
        mismatch = type("Mismatch", (), {"__version__": "2.6.0"})
        report = collect_report(FakeTorch, mismatch)
        self.assertFalse(report["checks"]["torchaudio_matches_torch"])

    def test_missing_torchaudio_is_reported(self):
        with patch("probe._import_torchaudio", return_value=None):
            report = collect_report(FakeTorch, None)
        self.assertFalse(report["checks"]["torchaudio_installed"])

    def test_cpu_only_runtime_does_not_claim_gpu_test(self):
        report = collect_report(FakeTorchCpu, FakeTorchaudio)
        self.assertFalse(report["checks"]["cuda_available"])
        self.assertFalse(report["checks"]["cuda_kernel_passed"])

    def test_cuda_kernel_failure_is_reported(self):
        report = collect_report(FakeTorchCudaFailure, FakeTorchaudio)
        self.assertFalse(report["checks"]["cuda_kernel_passed"])
        self.assertIn("cuda_error", report["pytorch"])

    def test_missing_torch_is_reported_without_crashing(self):
        with patch("builtins.__import__", side_effect=ModuleNotFoundError("torch missing")):
            report = _torch_report()
        self.assertFalse(report["installed"])
        self.assertFalse(report["cuda_available"])

    def test_probe_never_claims_inference_was_tested(self):
        report = collect_report(FakeTorch, FakeTorchaudio)
        self.assertFalse(report["model_loaded"])
        self.assertFalse(report["inference_tested"])
        self.assertFalse(report["network_used"])


if __name__ == "__main__":
    unittest.main()
