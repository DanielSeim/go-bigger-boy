#!/usr/bin/env python3
"""Focused tests for deterministic voxel corpus exploration selection."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "run_voxel_corpus.py"
SPEC = importlib.util.spec_from_file_location("run_voxel_corpus", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

ANALYZER_SCRIPT = Path(__file__).parents[1] / "scripts" / "voxel_scene_analyzer.py"
ANALYZER_SPEC = importlib.util.spec_from_file_location(
    "voxel_scene_analyzer", ANALYZER_SCRIPT
)
assert ANALYZER_SPEC and ANALYZER_SPEC.loader
ANALYZER = importlib.util.module_from_spec(ANALYZER_SPEC)
ANALYZER_SPEC.loader.exec_module(ANALYZER)


def record(frame: int, tile: int, buttons: str = "none") -> dict:
    return {
        "frame": frame,
        "input_buttons": buttons,
        "scene": {
            "visible_tile_cells": [{
                "screen_x": 0,
                "screen_y": 0,
                "tile_id": tile,
                "attributes": 0,
                "source": "background",
            }],
            "sprites": [],
        },
    }


class VoxelCorpusExplorationTests(unittest.TestCase):
    def test_variants_are_strictly_ordered_and_repeatable(self) -> None:
        for variant in range(3):
            first = MODULE.movie_events(180, variant)
            second = MODULE.movie_events(180, variant)
            self.assertEqual(first, second)
            self.assertEqual(first, sorted(first))
            self.assertEqual(len(first), len({frame for frame, _ in first}))

    def test_selection_discards_static_prefix_after_interaction(self) -> None:
        records = [record(frame, 0) for frame in range(35)]
        records.extend([
            record(35, 1, "a"),
            record(36, 2, "none"),
            record(37, 3, "none"),
            record(38, 4, "none"),
        ])
        selected, metrics = MODULE.select_gameplay_records(records)
        self.assertEqual(selected[0]["frame"], 25)
        self.assertEqual(metrics["frames_discarded"], 25)
        self.assertGreaterEqual(metrics["unique_scenes"], 4)

    def test_capture_score_rewards_input_response(self) -> None:
        static = [record(frame, 0, "a" if frame == 35 else "none") for frame in range(60)]
        changing = [
            record(frame, frame, "a" if frame == 35 else "none")
            for frame in range(60)
        ]
        self.assertGreater(
            MODULE.capture_metrics(changing)["score"],
            MODULE.capture_metrics(static)["score"],
        )

    def test_template_suggestions_scan_mixed_tile_layouts(self) -> None:
        layouts = (
            (4, 4, (7, 8, 9, 10)),
            (12, 4, (7, 8, 9, 10)),
        )
        cells = []
        for left, top, tiles in layouts:
            for y in range(top - 1, top + 3):
                for x in range(left - 1, left + 3):
                    if left <= x < left + 2 and top <= y < top + 2:
                        continue
                    cells.append({
                        "source": "background",
                        "screen_x": x * 8,
                        "screen_y": y * 8,
                        "visible_width": 8,
                        "visible_height": 8,
                        "map_x": x,
                        "map_y": y,
                        "map_address": 0x9800 + y * 32 + x,
                        "tile_id": 1,
                        "attributes": 0,
                        "opaque_mask": [255] * 8,
                    })
            for index, tile in enumerate(tiles):
                x = left + index % 2
                y = top + index // 2
                cells.append({
                    "source": "background",
                    "screen_x": x * 8,
                    "screen_y": y * 8,
                    "visible_width": 8,
                    "visible_height": 8,
                    "map_x": x,
                    "map_y": y,
                    "map_address": 0x9800 + y * 32 + x,
                    "tile_id": tile,
                    "attributes": 0,
                    "opaque_mask": [255] * 8,
                })
        records = [{
            "frame": frame,
            "rom_fingerprint": 123,
            "scene": {"visible_tile_cells": cells, "sprites": []},
        } for frame in range(3)]
        result = ANALYZER.analyze(records, minimum_capture_frames=3)
        self.assertEqual(result["schema"], "gbb.voxel.proposals.v2")
        self.assertTrue(result["template_proposals"])
        suggestion = result["template_proposals"][0]
        self.assertEqual(
            suggestion["profile_entry"],
            "background_object_template=2x2:7,8,9,10",
        )
        self.assertEqual(suggestion["locations"], 2)
        self.assertEqual(suggestion["observations"], 6)
        self.assertEqual(suggestion["decision"], "manual_review")

    def test_capture_command_includes_optional_save_inputs(self) -> None:
        command = MODULE.capture_cli_command(
            Path("gbb_cli"), Path("game.gbc"), Path("movie"), Path("scene"),
            600, 1000, battery_save_path=Path("game.sav"),
            state_path=Path("game.gbbstate"),
        )
        self.assertEqual(command[-4:], [
            "--battery-save", "game.sav", "--state", "game.gbbstate",
        ])

    def test_capture_input_prefers_save_beside_rom(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rom_dir = root / "roms"
            corpus_dir = root / "config"
            rom_dir.mkdir()
            corpus_dir.mkdir()
            rom_path = rom_dir / "game.gbc"
            rom_path.write_bytes(b"rom")
            (rom_dir / "game.sav").write_bytes(b"save")
            (corpus_dir / "game.sav").write_bytes(b"other")
            resolved = MODULE.resolve_capture_input(
                "game.sav", rom_path=rom_path,
                corpus_path=corpus_dir / "corpus.json",
                description="battery save",
            )
            self.assertEqual(resolved, rom_dir / "game.sav")


if __name__ == "__main__":
    unittest.main()
