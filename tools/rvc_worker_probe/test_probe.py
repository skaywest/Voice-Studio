import unittest
from probe import collect_report


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


class FakeTorch:
    __version__ = "2.7.1+cu128"
    version = type("Version", (), {"cuda": "12.8"})
    cuda = FakeCuda()


class ProbeTests(unittest.TestCase):
    def test_cuda_device_is_reported(self):
        report = collect_report(FakeTorch)
        self.assertTrue(report["pytorch"]["cuda_available"])
        self.assertEqual(report["pytorch"]["device"]["name"], "NVIDIA GeForce RTX 5060 Ti")
        self.assertEqual(report["pytorch"]["device"]["compute_capability"], "12.0")
        self.assertTrue(report["checks"]["cuda_128_build"])

    def test_probe_never_claims_model_was_loaded(self):
        report = collect_report(FakeTorch)
        self.assertFalse(report["model_loaded"])
        self.assertFalse(report["network_used"])

    def test_missing_torch_is_reported_without_crashing(self):
        report = collect_report(None)
        self.assertIn("installed", report["pytorch"])
        self.assertFalse(report["model_loaded"])


if __name__ == "__main__":
    unittest.main()
