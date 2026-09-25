"""ROM-free contracts for SGB scene alignment and exact matching."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from align_sgb_frames import exact_scenes, rank_frames, read_sgb_image  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
