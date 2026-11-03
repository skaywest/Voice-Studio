import tempfile
import unittest
from pathlib import Path

from realtime_worker_api import inspect_realtime_worker_api


COMPATIBLE_SOURCE = """
class RVCStreamEngine:
    def __init__(self, cfg):
        pass

    def prewarm(self):
        pass

    def process(self, audio, pitch, formant, index_rate, rms_mix, threshold, f0_method):
        return audio
"""


class RealtimeWorkerApiTests(unittest.TestCase):
    def inspect(self, source):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "rvc_worker.py"
            path.write_text(source, encoding="utf-8")
            return inspect_realtime_worker_api(path)

    def test_expected_upstream_streaming_api_is_accepted(self):
        report = self.inspect(COMPATIBLE_SOURCE)
        self.assertTrue(report["available"])
        self.assertTrue(report["engine_class_found"])
        self.assertTrue(report["compatible"])
        self.assertFalse(report["block_contract_verified"])

    def test_missing_prewarm_method_is_rejected(self):
        source = COMPATIBLE_SOURCE.replace("    def prewarm(self):\\n        pass\\n\\n", "")
        report = self.inspect(source)
        self.assertFalse(report["compatible"])
        self.assertFalse(report["prewarm"]["present"])

    def test_incompatible_process_signature_is_rejected(self):
        report = self.inspect(
            COMPATIBLE_SOURCE.replace("f0_method", "pitch_method")
        )
        self.assertTrue(report["available"])
        self.assertFalse(report["compatible"])

    def test_required_keyword_only_argument_is_rejected(self):
        source = COMPATIBLE_SOURCE.replace(
            "f0_method):", "f0_method, *, sample_rate):"
        )
        self.assertFalse(self.inspect(source)["compatible"])

    def test_variadic_process_signature_is_rejected(self):
        source = COMPATIBLE_SOURCE.replace(
            "f0_method):", "f0_method, *args, **kwargs):"
        )
        self.assertFalse(self.inspect(source)["compatible"])

    def test_async_process_is_rejected(self):
        source = COMPATIBLE_SOURCE.replace(
            "    def process(", "    async def process("
        )
        self.assertFalse(self.inspect(source)["compatible"])

    def test_static_process_is_rejected(self):
        source = COMPATIBLE_SOURCE.replace(
            "    def process(", "    @staticmethod\n    def process("
        )
        self.assertFalse(self.inspect(source)["compatible"])

    def test_missing_engine_class_is_rejected(self):
        report = self.inspect("class OtherEngine:\n    pass\n")
        self.assertFalse(report["available"])
        self.assertFalse(report["compatible"])

    def test_missing_file_and_invalid_syntax_are_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            missing = inspect_realtime_worker_api(Path(temp) / "missing.py")
            self.assertFalse(missing["available"])
            invalid = Path(temp) / "invalid.py"
            invalid.write_text("class RVCStreamEngine(:", encoding="utf-8")
            malformed = inspect_realtime_worker_api(invalid)
            self.assertFalse(malformed["available"])
            self.assertIn("SyntaxError", malformed["error"])

    def test_source_is_parsed_without_execution(self):
        report = self.inspect(
            "raise RuntimeError('must not execute')\n" + COMPATIBLE_SOURCE
        )
        self.assertTrue(report["compatible"])


if __name__ == "__main__":
    unittest.main()
