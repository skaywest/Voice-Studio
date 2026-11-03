import tempfile
import unittest
from pathlib import Path

from realtime_worker_api import inspect_realtime_worker_api


COMPATIBLE_SOURCE = """
class RVCStreamEngine:
    def __init__(self, cfg):
        pass

    def process(self, audio, pitch, formant, index_rate, rms_mix, threshold, f0_method):
        return audio
"""


class RealtimeWorkerApiTests(unittest.TestCase):
    def test_expected_upstream_streaming_api_is_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "rvc_worker.py"
            path.write_text(COMPATIBLE_SOURCE, encoding="utf-8")
            report = inspect_realtime_worker_api(path)
            self.assertTrue(report["available"])
            self.assertTrue(report["engine_class_found"])
            self.assertTrue(report["compatible"])

    def test_incompatible_process_signature_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "rvc_worker.py"
            path.write_text(
                COMPATIBLE_SOURCE.replace("f0_method", "pitch_method"),
                encoding="utf-8",
            )
            report = inspect_realtime_worker_api(path)
            self.assertTrue(report["available"])
            self.assertFalse(report["compatible"])

    def test_missing_engine_class_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "rvc_worker.py"
            path.write_text("class OtherEngine:" + chr(10) + "    pass" + chr(10), encoding="utf-8")
            report = inspect_realtime_worker_api(path)
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
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "rvc_worker.py"
            path.write_text(
                "raise RuntimeError('must not execute')" + chr(10) + COMPATIBLE_SOURCE,
                encoding="utf-8",
            )
            report = inspect_realtime_worker_api(path)
            self.assertTrue(report["compatible"])


if __name__ == "__main__":
    unittest.main()
