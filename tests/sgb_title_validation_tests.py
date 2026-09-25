"""Contract tests for the opt-in, non-proprietary SGB title validator."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from validate_sgb_title import compare_reference, load_manifest, validate  # noqa: E402
from sgb_trace_fixture_test import load_fixture  # noqa: E402


RUNNER = Path(sys.argv.pop()) if len(sys.argv) > 1 else None


class SgbTitleValidationTests(unittest.TestCase):
    def test_fixture_inventory_is_not_mislabeled_as_reference(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        fixture = Path(__file__).parent / "fixtures/sgb/trace_fixture.hex"
        rom_bytes = load_fixture(fixture)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "fixture.gb"
            rom.write_bytes(rom_bytes)
            manifest = root / "case.json"
            manifest.write_text(json.dumps({
                "schema": 1,
                "title": "Clean-room packet fixture",
                "rom_sha256": hashlib.sha256(rom_bytes).hexdigest(),
                "model": "sgb",
                "frames": 2,
                "max_cycles": 500_000,
                "command_minimums": {"0x11": 1},
                "reference": None,
            }), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "capture")
            self.assertEqual(report["status"], "inventory_only")
            self.assertEqual(report["commands"]["0x11"]["count"], 1)
            self.assertTrue((root / "capture/sgb-frame.ppm").read_bytes().startswith(
                b"P6\n256 224\n255\n"))
            self.assertTrue((root / "capture/report.json").is_file())
            manifest_data = json.loads(manifest.read_text())
            manifest_data["rom_sha256"] = "0" * 64
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                validate(manifest, rom, RUNNER, root / "bad")

    def test_full_and_viewport_reference_detect_differences(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            actual = root / "actual.ppm"
            whole = bytearray(256 * 224 * 3)
            viewport = bytearray(160 * 144 * 3)
            actual.write_bytes(b"P6\n256 224\n255\n" + whole)
            reference = root / "reference.ppm"
            reference.write_bytes(b"P6\n160 144\n255\n" + viewport)
            self.assertTrue(compare_reference(actual, reference, "viewport", 0, 0)["passed"])
            whole[(40 * 256 + 48) * 3] = 7
            actual.write_bytes(b"P6\n256 224\n255\n" + whole)
            comparison = compare_reference(actual, reference, "viewport", 0, 0)
            self.assertFalse(comparison["passed"])
            self.assertEqual(comparison["first_mismatch"], [0, 0])
            self.assertTrue(compare_reference(actual, reference, "viewport", 7, 0)["passed"])
            with self.assertRaisesRegex(ValueError, "full reference"):
                compare_reference(actual, reference, "full", 0, 0)

    def test_reference_pipeline_checks_digest_and_pixels(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        rom_bytes = load_fixture(Path(__file__).parent /
                                 "fixtures/sgb/trace_fixture.hex")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "fixture.gb"
            rom.write_bytes(rom_bytes)
            manifest = root / "case.json"
            data = {
                "schema": 1, "title": "Synthetic reference plumbing test",
                "rom_sha256": hashlib.sha256(rom_bytes).hexdigest(),
                "model": "sgb", "frames": 2, "max_cycles": 500_000,
                "command_minimums": {"0x11": 1},
            }
            manifest.write_text(json.dumps(data), encoding="utf-8")
            inventory = validate(manifest, rom, RUNNER, root / "inventory")
            reference = root / "reference.ppm"
            reference.write_bytes(Path(inventory["frame"]).read_bytes())
            data["reference"] = {
                "source": "independent-emulator",
                "description": "Synthetic test of comparison plumbing only",
                "image": "reference.ppm", "region": "full",
                "image_sha256": hashlib.sha256(reference.read_bytes()).hexdigest(),
            }
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "matching")
            self.assertEqual(report["status"], "validated")
            self.assertEqual(report["reference"]["comparison"]["mismatched_pixels"], 0)
            changed = bytearray(reference.read_bytes())
            changed[-1] ^= 0xFF
            reference.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, "reference image SHA-256"):
                validate(manifest, rom, RUNNER, root / "stale")
            data["reference"]["image_sha256"] = hashlib.sha256(changed).hexdigest()
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "different")
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["reference"]["comparison"]["mismatched_pixels"], 1)

    def test_manifest_rejects_unpinned_or_unattributed_results(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "case.json"
            base = {
                "schema": 1, "title": "Example", "model": "sgb",
                "rom_sha256": "a" * 64, "frames": 3,
                "max_cycles": 500_000, "command_minimums": {"0x11": 1},
            }
            manifest.write_text(json.dumps({**base, "reference": {
                "source": "gbb", "description": "self-generated",
                "image": "self.ppm", "region": "full",
            }}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "reference source"):
                load_manifest(manifest)
            manifest.write_text(json.dumps({**base, "rom_sha256": "bad"}),
                                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                load_manifest(manifest)
            manifest.write_text(json.dumps({**base, "schema": True}),
                                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "schema 1"):
                load_manifest(manifest)

    def test_full_sgb_capture_rejects_other_hardware(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        rom_bytes = load_fixture(Path(__file__).parent /
                                 "fixtures/sgb/trace_fixture.hex")
        with tempfile.TemporaryDirectory() as directory:
            rom = Path(directory) / "fixture.gb"
            rom.write_bytes(rom_bytes)
            result = subprocess.run(
                [str(RUNNER.resolve()), str(rom), "--model", "dmg",
                 "--frames", "1", "--max-cycles", "500000",
                 "--sgb-frame", "--frame-output",
                 str(Path(directory) / "invalid.ppm")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("requires an SGB or SGB2", result.stderr)

    def test_sgb_frame_series_matches_single_capture(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        rom_bytes = load_fixture(Path(__file__).parent /
                                 "fixtures/sgb/trace_fixture.hex")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "fixture.gb"
            rom.write_bytes(rom_bytes)
            common = [str(RUNNER.resolve()), str(rom), "--model", "sgb",
                      "--max-cycles", "500000", "--sgb-frame"]
            series = subprocess.run(
                common + ["--frame-series", "1", "2", str(root / "series")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(series.returncode, 0, series.stderr)
            self.assertTrue((root / "series-1.ppm").is_file())
            single = subprocess.run(
                common + ["--frames", "2", "--frame-output",
                          str(root / "single.ppm")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(single.returncode, 0, single.stderr)
            self.assertEqual((root / "series-2.ppm").read_bytes(),
                             (root / "single.ppm").read_bytes())
            conflicting = subprocess.run(
                common + ["--frame-series", "1", "2", str(root / "bad"),
                          "--frames", "2", "--frame-output",
                          str(root / "bad-single.ppm")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertNotEqual(conflicting.returncode, 0)
            self.assertIn("mutually exclusive", conflicting.stderr)
            self.assertFalse((root / "bad-1.ppm").exists())


if __name__ == "__main__":
    unittest.main()
