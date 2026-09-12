#!/usr/bin/env python3
"""Focused tests for deterministic voxel corpus exploration selection."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "run_voxel_corpus.py"
SPEC = importlib.util.spec_from_file_location("run_voxel_corpus", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


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


if __name__ == "__main__":
    unittest.main()
