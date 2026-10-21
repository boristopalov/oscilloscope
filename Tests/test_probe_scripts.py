import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("analyzer", ROOT / "scripts/analyze_probes.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


class ProbeScriptsTests(unittest.TestCase):
    def test_raw_residual_is_retained_when_provisional_threshold_rejects_it(self):
        row = dict(schema="1", process_id="7", registry_token="registry", source_id="1", generation="2",
                   event="discontinuity", dropped_total="0", dropped_session="0", evicted_total="0", playing="1", looping="0",
                   sample_error="0", ppq_error_samples="2.25", sample_position="48000", peak_offset_l="7")
        summary = analyzer.summarize([row])
        self.assertIn("max magnitude 2.25 samples", summary)
        self.assertIn("48007", summary)
        self.assertIn("Registries observed: 1", summary)

    def test_unknown_schema_fails(self):
        with self.assertRaises(ValueError):
            analyzer.summarize([{"schema": "2"}])

    def test_fixture_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([sys.executable, str(ROOT / "scripts/make_probe_audio.py"), directory], check=True,
                           capture_output=True)
            with wave.open(str(pathlib.Path(directory) / "impulses-48k.wav")) as fixture:
                self.assertEqual((fixture.getframerate(), fixture.getnchannels(), fixture.getnframes()),
                                 (48000, 2, 384000))
                fixture.setpos(4799)
                self.assertEqual(fixture.readframes(3), b"\x00\x00\x00\x00\x00\x60\x00\x60\x00\x00\x00\x00")


if __name__ == "__main__":
    unittest.main()
