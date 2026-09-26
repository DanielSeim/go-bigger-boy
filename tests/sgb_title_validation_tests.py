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
    def test_checked_in_title_manifests_pin_existing_input_scripts(self) -> None:
        title_dir = Path(__file__).parent / "fixtures/sgb/titles"
        manifests = sorted(title_dir.glob("*.json"))
        self.assertTrue(manifests)
        for path in manifests:
            with self.subTest(manifest=path.name):
                data = load_manifest(path)
                input_data = data.get("input")
                if input_data is not None:
                    script = title_dir / input_data["script"]
                    self.assertTrue(script.is_file())
                    self.assertEqual(hashlib.sha256(script.read_bytes()).hexdigest(),
                                     input_data["script_sha256"])

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
            data["reference"] = {
                "source": "independent-emulator",
                "description": "Synthetic test of digest comparison plumbing only",
                "frame_sha256": inventory["frame_sha256"],
            }
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "digest-match")
            self.assertEqual(report["status"], "validated")
            self.assertTrue(report["reference"]["comparison"]["exact_frame_digest"])
            data["reference"]["frame_sha256"] = "0" * 64
            manifest.write_text(json.dumps(data), encoding="utf-8")
            self.assertEqual(validate(manifest, rom, RUNNER,
                                      root / "digest-mismatch")["status"], "failed")

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

    def test_manifest_pins_and_replays_input_script(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        fixture_dir = Path(__file__).parent / "fixtures/sgb"
        rom_bytes = load_fixture(fixture_dir / "trace_fixture.hex")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "fixture.gb"
            rom.write_bytes(rom_bytes)
            script = root / "input.script"
            script.write_bytes((fixture_dir / "input_fixture.script").read_bytes())
            manifest = root / "case.json"
            data = {
                "schema": 1, "title": "Synthetic scripted inventory",
                "rom_sha256": hashlib.sha256(rom_bytes).hexdigest(),
                "model": "sgb", "frames": 5, "max_cycles": 500_000,
                "command_minimums": {"0x11": 1},
                "input": {"script": script.name,
                          "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest()},
            }
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "capture")
            self.assertEqual(report["status"], "inventory_only")
            self.assertEqual(report["input_script_sha256"], data["input"]["script_sha256"])
            script.write_text("GBB SGB input v1\n0 start\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "input script SHA-256"):
                validate(manifest, rom, RUNNER, root / "stale")
            self.assertFalse((root / "stale").exists())

    def test_checkpoint_sequence_reports_first_divergent_frame(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        fixture_dir = Path(__file__).parent / "fixtures/sgb"
        rom_bytes = load_fixture(fixture_dir / "trace_fixture.hex")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "fixture.gb"
            rom.write_bytes(rom_bytes)
            script = fixture_dir / "input_fixture.script"
            capture = subprocess.run(
                [str(RUNNER.resolve()), str(rom), "--model", "sgb",
                 "--sgb-frame", "--max-cycles", "500000",
                 "--input-script", str(script), "--frame-series", "2", "3",
                 str(root / "reference")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(capture.returncode, 0, capture.stderr)
            checkpoints = [
                {"frame": frame, "reference_frame": frame + 100,
                 "frame_sha256": hashlib.sha256(
                     (root / f"reference-{frame}.ppm").read_bytes()).hexdigest()}
                for frame in (2, 3)
            ]
            data = {
                "schema": 1, "title": "Synthetic checkpoint plumbing only",
                "rom_sha256": hashlib.sha256(rom_bytes).hexdigest(),
                "model": "sgb", "frames": 3, "max_cycles": 500_000,
                "command_minimums": {"0x11": 1},
                "input": {"script": str(script),
                          "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest()},
                "reference": {"source": "independent-emulator",
                              "description": "Synthetic comparison plumbing only",
                              "checkpoints": checkpoints},
            }
            manifest = root / "case.json"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "matching")
            self.assertEqual(report["status"], "validated")
            self.assertEqual(report["reference"]["comparison"]["matched_frames"], 2)
            self.assertTrue((root / "matching/sgb-frame-2.ppm").is_file())
            checkpoints[0]["frame_sha256"] = "0" * 64
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = validate(manifest, rom, RUNNER, root / "different")
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["reference"]["comparison"]["first_mismatch"]["frame"], 2)
            self.assertEqual(report["reference"]["comparison"]["matched_frames"], 1)
            checkpoints[0]["frame"] = 3
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "must increase"):
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
                common + ["--frame-series", "1", "2", str(root / "series"),
                          "--frame-state-series", "--watch-wram", "0xC000"],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(series.returncode, 0, series.stderr)
            self.assertTrue((root / "series-1.ppm").is_file())
            self.assertEqual((root / "series-1.state").stat().st_size,
                             0x2000 + 0xA0)
            self.assertEqual((root / "series-2.state").stat().st_size,
                             0x2000 + 0xA0)
            for frame in (1, 2):
                metadata = (root / f"series-{frame}.meta").read_text()
                self.assertIn(f"frame={frame} cycles=", metadata)
                for register in ("pc", "sp", "af", "bc", "de", "hl",
                                 "ff40", "ff41", "ff44", "ff45", "ff04",
                                 "ff0f", "ffff"):
                    self.assertRegex(metadata, rf"\b{register}=[0-9a-f]+\b")
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
            for invalid in (["--frame-state-series"],
                            ["--watch-wram", "0xBFFF"],
                            ["--watch-wram", "0xC000"]):
                rejected = subprocess.run(
                    common + ["--frames", "1", "--frame-output",
                              str(root / "invalid.ppm")] + invalid,
                    capture_output=True, text=True, timeout=10, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertFalse((root / "invalid.ppm").exists())

    def test_input_script_changes_and_restores_synthetic_scene(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        fixture_dir = Path(__file__).parent / "fixtures/sgb"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "input.gb"
            rom.write_bytes(load_fixture(fixture_dir / "input_fixture.hex"))
            command = [str(RUNNER.resolve()), str(rom), "--model", "sgb",
                       "--max-cycles", "1000000", "--sgb-frame"]
            scripted = subprocess.run(
                command + ["--input-script", str(fixture_dir / "input_fixture.script"),
                           "--frame-series", "1", "8", str(root / "scripted")],
                capture_output=True, text=True, timeout=15, check=False)
            self.assertEqual(scripted.returncode, 0, scripted.stderr)
            plain = subprocess.run(
                command + ["--frame-series", "1", "8", str(root / "plain")],
                capture_output=True, text=True, timeout=15, check=False)
            self.assertEqual(plain.returncode, 0, plain.stderr)
            scripted_frames = [(root / f"scripted-{frame}.ppm").read_bytes()
                               for frame in range(1, 9)]
            plain_frames = [(root / f"plain-{frame}.ppm").read_bytes()
                            for frame in range(1, 9)]
            self.assertEqual(scripted_frames[:2], plain_frames[:2])
            self.assertTrue(any(a != b for a, b in
                                zip(scripted_frames[2:5], plain_frames[2:5])))
            self.assertEqual(scripted_frames[-1], plain_frames[-1])

            before_first = root / "before-first.script"
            before_first.write_text("GBB SGB input v1\n0 a+start\n4 none\n",
                                    encoding="utf-8")
            first = subprocess.run(
                command + ["--input-script", str(before_first),
                           "--frame-series", "1", "8", str(root / "early")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertTrue(any((root / f"early-{frame}.ppm").read_bytes() !=
                                plain_frames[frame - 1]
                                for frame in range(1, 9)),
                            "startup input must affect a displayed complete frame")

            for malformed, message in [
                ("bad header\n", "header"),
                ("GBB SGB input v1\n2 a\n2 none\n", "increase"),
                ("GBB SGB input v1\n2 fire\n", "unknown"),
                ("GBB SGB input v1\n2 a+a\n", "duplicate"),
            ]:
                script = root / "bad.script"
                script.write_text(malformed, encoding="utf-8")
                result = subprocess.run(
                    command + ["--input-script", str(script), "--frames", "3",
                               "--frame-output", str(root / "bad.ppm")],
                    capture_output=True, text=True, timeout=10, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)


if __name__ == "__main__":
    unittest.main()
