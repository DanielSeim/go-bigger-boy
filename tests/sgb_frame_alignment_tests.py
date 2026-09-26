"""ROM-free contracts for SGB scene alignment and exact matching."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from align_sgb_frames import (compare_sequences, exact_scenes, rank_frames,
                              read_sgb_image)  # noqa: E402


def frame(path: Path, color: tuple[int, int, int], offset: int = 0) -> None:
    pixels = bytearray(256 * 224 * 3)
    for y in range(60, 100):
        for x in range(70 + offset, 110 + offset):
            index = (y * 256 + x) * 3
            pixels[index:index + 3] = bytes(color)
    path.write_bytes(b"P6\n256 224\n255\n" + pixels)


class SgbFrameAlignmentTests(unittest.TestCase):
    def test_exact_scene_matches_without_rom_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "gbb-120.ppm"
            matching = root / "reference-351.ppm"
            different = root / "reference-350.ppm"
            frame(target, (10, 20, 30))
            frame(matching, (10, 20, 30))
            frame(different, (20, 30, 40))
            matches = exact_scenes([target], [different, matching])
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["reference"], str(matching))
            self.assertEqual(matches[0]["target_count"], 1)
            self.assertEqual(exact_scenes([target], [different]), [])

    def test_edges_align_scene_even_when_colors_differ(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.ppm"
            recolored = root / "recolored.ppm"
            shifted = root / "shifted.ppm"
            frame(target, (10, 20, 30))
            frame(recolored, (60, 70, 80))
            frame(shifted, (10, 20, 30), offset=4)
            results = rank_frames(target, [shifted, recolored])
            self.assertEqual(results[0]["reference"], str(recolored))
            self.assertEqual(results[0]["viewport_edge_mismatches"], 0)
            self.assertGreater(results[0]["rgb_mismatches"], 0)
            self.assertEqual(rank_frames(target, [shifted, recolored], "rgb")[0]
                             ["reference"], str(shifted))
            with self.assertRaisesRegex(ValueError, "priority"):
                rank_frames(target, [shifted], "invalid")

    def test_rejects_non_sgb_frame(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "small.ppm"
            path.write_bytes(b"P6\n1 1\n255\n" + bytes(3))
            with self.assertRaisesRegex(ValueError, "256x224"):
                read_sgb_image(path)

    def test_sequence_reports_first_divergent_frame_after_bounded_alignment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            targets = [root / f"gbb-{number}.ppm" for number in (10, 11, 12)]
            references = [root / f"sameboy-{number}.ppm"
                          for number in (110, 111, 112, 113)]
            frame(targets[0], (10, 20, 30))
            frame(targets[1], (40, 50, 60), offset=2)
            frame(targets[2], (70, 80, 90), offset=4)
            frame(references[0], (10, 20, 30))
            frame(references[1], (10, 20, 30))
            frame(references[2], (40, 50, 60), offset=2)
            frame(references[3], (1, 2, 3))
            result = compare_sequences(targets, references, 100, 1)
            self.assertFalse(result["passed"])
            self.assertEqual(result["matched_frames"], 2)
            self.assertEqual(result["first_mismatch"]["frame"], 12)
            self.assertEqual(result["first_mismatch"]["nearest_reference_frame"], 112)
            self.assertGreater(result["first_mismatch"]["mismatched_pixels"], 0)
            self.assertEqual(result["unique_target_scenes"], 3)
            self.assertEqual(result["largest_matched_shift"], 1)
            self.assertEqual(compare_sequences(targets[:2], references[:3],
                                               100, 1)["unmatched_frames"], 0)
            completed = subprocess.run(
                [sys.executable,
                 str(Path(__file__).resolve().parents[1] / "scripts/align_sgb_frames.py"),
                 "--target-series", str(root / "gbb-*.ppm"),
                 "--reference-series", str(root / "sameboy-*.ppm"),
                 "--sequence-offset", "100", "--window", "1"],
                capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 1)
            self.assertEqual(json.loads(completed.stdout)["first_mismatch"]["frame"], 12)
            with self.assertRaisesRegex(ValueError, "window"):
                compare_sequences(targets, references, 100, 61)
            self.assertEqual(compare_sequences(targets + [targets[0]],
                                               references, 100, 1)["target_frames"], 3)

    def test_sequence_phase_changes_and_identical_chunk_overlap(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            targets = [root / f"gbb-a-{number}.ppm" for number in (10, 11)] + [
                root / "gbb-b-12.ppm"]
            references = [root / f"ref-a-{number}.ppm" for number in (110, 111)] + [
                root / "ref-b-212.ppm"]
            for index, (target, reference) in enumerate(zip(targets, references)):
                color = (10 + index, 20 + index, 30 + index)
                frame(target, color)
                frame(reference, color)
            overlap = root / "gbb-b-11.ppm"
            overlap.write_bytes(targets[1].read_bytes())
            result = compare_sequences(targets + [overlap], references, 100, 0,
                                       [(12, 200)])
            self.assertTrue(result["passed"])
            self.assertEqual(result["target_frames"], 3)
            self.assertEqual(result["offset_changes"], [{"frame": 12, "offset": 200}])
            completed = subprocess.run(
                [sys.executable,
                 str(Path(__file__).resolve().parents[1] / "scripts/align_sgb_frames.py"),
                 "--target-series", str(root / "gbb-a-*.ppm"),
                 "--target-series", str(root / "gbb-b-*.ppm"),
                 "--reference-series", str(root / "ref-a-*.ppm"),
                 "--reference-series", str(root / "ref-b-*.ppm"),
                 "--sequence-offset", "100", "--sequence-offset-at", "12", "200"],
                capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(completed.stdout)["matched_frames"], 3)
            with self.assertRaisesRegex(ValueError, "strictly increasing"):
                compare_sequences(targets, references, 100, 0,
                                  [(12, 200), (11, 200)])
            frame(overlap, (100, 110, 120))
            with self.assertRaisesRegex(ValueError, "conflicting capture"):
                compare_sequences(targets + [overlap], references, 100, 0,
                                  [(12, 200)])


if __name__ == "__main__":
    unittest.main()
