import math
import tempfile
import unittest
from pathlib import Path

from stream_contract import (
    HOST_BLOCK_FRAMES,
    HOST_SAMPLE_RATE,
    RvcRealtimeAdapter,
    StreamContractError,
    build_engine_config,
    expected_rvc_block_frames,
)


class FakeEngine:
    sample_rate = HOST_SAMPLE_RATE
    block_frame = HOST_BLOCK_FRAMES

    def __init__(self, output=None):
        self.output = output
        self.calls = 0
        self.warmed = False

    def prewarm(self):
        self.warmed = True

    def process(self, audio, pitch, formant, index_rate, rms_mix, threshold, f0_method):
        self.calls += 1
        return audio if self.output is None else self.output


class StreamContractTests(unittest.TestCase):
    @staticmethod
    def make_rvc_root(parent):
        root = Path(parent) / "rvc"
        worker = root / "RVCRealtimeVST" / "worker" / "rvc_worker.py"
        worker.parent.mkdir(parents=True)
        worker.write_text("# externally supplied RVC worker fixture\\n", encoding="utf-8")
        return root

    def test_upstream_rounding_matches_native_60ms_mono_48khz_block(self):
        self.assertEqual(expected_rvc_block_frames(48_000, 60.0), 2_880)
        self.assertEqual(expected_rvc_block_frames(48_000, 40.0), 1_920)

    def test_invalid_geometry_inputs_are_rejected(self):
        for rate, duration in ((0, 60.0), (48_000, 0), (48_000, math.inf), (True, 60.0)):
            with self.subTest(rate=rate, duration=duration):
                with self.assertRaises(ValueError):
                    expected_rvc_block_frames(rate, duration)

    def test_engine_config_explicitly_targets_native_block_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            root = self.make_rvc_root(temp)
            model = Path(temp) / "voice.pth"
            model.write_bytes(b"fixture")
            config = build_engine_config(root, model)
        self.assertEqual(config["sample_rate"], 48_000)
        self.assertEqual(config["block_ms"], 60.0)
        self.assertEqual(config["crossfade_ms"], 40.0)
        self.assertEqual(config["extra_ms"], 2_000.0)
        self.assertEqual(config["index_path"], "")
        self.assertEqual(
            expected_rvc_block_frames(config["sample_rate"], config["block_ms"]),
            HOST_BLOCK_FRAMES,
        )

    def test_config_rejects_missing_or_wrong_model(self):
        with tempfile.TemporaryDirectory() as temp:
            root = self.make_rvc_root(temp)
            with self.assertRaises(ValueError):
                build_engine_config(root, Path(temp) / "missing.pth")
            wrong = Path(temp) / "voice.bin"
            wrong.write_bytes(b"fixture")
            with self.assertRaises(ValueError):
                build_engine_config(root, wrong)
            empty = Path(temp) / "empty.pth"
            empty.touch()
            with self.assertRaisesRegex(ValueError, "must not be empty"):
                build_engine_config(root, empty)

    def test_config_rejects_directory_without_rvc_worker_source(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "not-rvc"
            root.mkdir()
            model = Path(temp) / "voice.pth"
            model.write_bytes(b"fixture")
            with self.assertRaisesRegex(ValueError, "RVCRealtimeVST/worker/rvc_worker.py"):
                build_engine_config(root, model)

    def test_config_rejects_empty_index(self):
        with tempfile.TemporaryDirectory() as temp:
            root = self.make_rvc_root(temp)
            model = Path(temp) / "voice.pth"
            model.write_bytes(b"fixture")
            index = Path(temp) / "empty.index"
            index.touch()
            with self.assertRaisesRegex(ValueError, "index_path must not be empty"):
                build_engine_config(root, model, index)

    def test_adapter_checks_engine_geometry_before_start(self):
        engine = FakeEngine()
        engine.block_frame = 1_440
        with self.assertRaises(StreamContractError):
            RvcRealtimeAdapter(engine)

        engine = FakeEngine()
        engine.sample_rate = 44_100
        with self.assertRaises(StreamContractError):
            RvcRealtimeAdapter(engine)

    def test_adapter_prewarm_and_exact_block_round_trip(self):
        engine = FakeEngine()
        adapter = RvcRealtimeAdapter(engine)
        adapter.prewarm()
        self.assertTrue(engine.warmed)
        audio = [0.125] * HOST_BLOCK_FRAMES
        output = adapter.process(audio, 0.0, 0.0, 0.0, 0.5, -60.0, 0)
        self.assertIs(output, audio)
        self.assertEqual(engine.calls, 1)

    def test_adapter_rejects_wrong_input_and_output_lengths(self):
        adapter = RvcRealtimeAdapter(FakeEngine())
        with self.assertRaises(StreamContractError):
            adapter.process([0.0] * 100, 0, 0, 0, 0.5, -60, 0)

        engine = FakeEngine(output=[0.0] * (HOST_BLOCK_FRAMES - 1))
        adapter = RvcRealtimeAdapter(engine)
        with self.assertRaises(StreamContractError):
            adapter.process([0.0] * HOST_BLOCK_FRAMES, 0, 0, 0, 0.5, -60, 0)

    def test_adapter_rejects_unsized_input(self):
        adapter = RvcRealtimeAdapter(FakeEngine())
        with self.assertRaisesRegex(StreamContractError, "sized, iterable"):
            adapter.process(iter([0.0] * HOST_BLOCK_FRAMES), 0, 0, 0, 0.5, -60, 0)

    def test_adapter_rejects_iterators_whose_length_disagrees_with_len(self):
        class InconsistentBuffer:
            def __init__(self, reported_length, samples):
                self.reported_length = reported_length
                self.samples = samples

            def __len__(self):
                return self.reported_length

            def __iter__(self):
                return iter(self.samples)

        engine = FakeEngine()
        adapter = RvcRealtimeAdapter(engine)
        short_input = InconsistentBuffer(HOST_BLOCK_FRAMES, [0.0] * (HOST_BLOCK_FRAMES - 1))
        with self.assertRaisesRegex(StreamContractError, "input iterator yielded"):
            adapter.process(short_input, 0, 0, 0, 0.5, -60, 0)
        self.assertEqual(engine.calls, 0)

        long_output = InconsistentBuffer(HOST_BLOCK_FRAMES, [0.0] * (HOST_BLOCK_FRAMES + 1))
        with self.assertRaisesRegex(StreamContractError, "output iterator yielded more"):
            RvcRealtimeAdapter(FakeEngine(output=long_output)).process(
                [0.0] * HOST_BLOCK_FRAMES, 0, 0, 0, 0.5, -60, 0
            )

    def test_adapter_rejects_non_finite_input_samples_before_engine_call(self):
        audio = [0.0] * HOST_BLOCK_FRAMES
        audio[17] = float("inf")
        engine = FakeEngine()
        adapter = RvcRealtimeAdapter(engine)
        with self.assertRaisesRegex(StreamContractError, "input block.*index 17"):
            adapter.process(audio, 0, 0, 0, 0.5, -60, 0)
        self.assertEqual(engine.calls, 0)

    def test_adapter_rejects_non_finite_output_samples(self):
        output = [0.0] * HOST_BLOCK_FRAMES
        output[123] = float("nan")
        adapter = RvcRealtimeAdapter(FakeEngine(output=output))
        with self.assertRaisesRegex(StreamContractError, "index 123"):
            adapter.process([0.0] * HOST_BLOCK_FRAMES, 0, 0, 0, 0.5, -60, 0)


if __name__ == "__main__":
    unittest.main()
