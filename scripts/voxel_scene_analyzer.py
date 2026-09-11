#!/usr/bin/env python3
"""Generate deterministic pop-up object proposals from scene observation JSONL.

This is intentionally a proposal tool, not a renderer and not a machine-learning
runtime. It uses visible BG tile provenance, simple tile-graph segmentation and
explainable features so a future debug UI can accept, reject, split or merge the
results into ROM-specific voxel profile rules.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


def bit_count(value: int) -> int:
    return bin(value & 0xFF).count("1")


def density(cell: dict[str, Any]) -> float:
    rows = cell.get("opaque_mask", [])
    return sum(bit_count(int(row)) for row in rows) / 64.0


def mask_distance(left: dict[str, Any], right: dict[str, Any]) -> float:
    rows_left = left.get("opaque_mask", [])
    rows_right = right.get("opaque_mask", [])
    return sum(
        bit_count(int(a) ^ int(b))
        for a, b in zip(rows_left, rows_right)
    ) / 64.0


def tile_signature(cell: dict[str, Any]) -> tuple[Any, ...]:
    return (
        int(cell.get("tile_id", 0)),
        int(cell.get("attributes", 0)) & 0x78,
        tuple(int(row) for row in cell.get("opaque_mask", [])),
    )


def edge_cost(left: dict[str, Any], right: dict[str, Any]) -> int:
    if tile_signature(left) == tile_signature(right):
        return 0
    if (
        int(left.get("tile_id", 0)) == int(right.get("tile_id", 0))
        and (int(left.get("attributes", 0)) & 0x78)
        == (int(right.get("attributes", 0)) & 0x78)
    ):
        return 1
    if (
        abs(density(left) - density(right)) <= 0.125
        and mask_distance(left, right) <= 0.35
    ):
        return 2
    return 3


def visible_cell_key(cell: dict[str, Any]) -> tuple[int, int]:
    return int(cell.get("screen_x", 0)), int(cell.get("screen_y", 0))


def overlaps_sprite(cell: dict[str, Any], sprite: dict[str, Any]) -> bool:
    cell_left = int(cell.get("screen_x", 0))
    cell_top = int(cell.get("screen_y", 0))
    cell_right = cell_left + int(cell.get("visible_width", 8))
    cell_bottom = cell_top + int(cell.get("visible_height", 8))
    sprite_left = int(sprite.get("screen_x", 0))
    sprite_top = int(sprite.get("screen_y", 0))
    sprite_height = 16 if int(sprite.get("attributes", 0)) & 0x04 else 8
    sprite_right = sprite_left + 8
    sprite_bottom = sprite_top + sprite_height
    return (
        cell_left < sprite_right
        and cell_right > sprite_left
        and cell_top < sprite_bottom
        and cell_bottom > sprite_top
    )


def union_find_components(cells: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    by_position = {visible_cell_key(cell): cell for cell in cells}
    parent = {position: position for position in by_position}

    def find(position: tuple[int, int]) -> tuple[int, int]:
        while parent[position] != position:
            parent[position] = parent[parent[position]]
            position = parent[position]
        return position

    def join(left: tuple[int, int], right: tuple[int, int]) -> None:
        left_root = find(left)
        right_root = find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    for x, y in by_position:
        for neighbour in ((x + 8, y), (x, y + 8)):
            if neighbour in by_position and edge_cost(
                by_position[(x, y)], by_position[neighbour]
            ) <= 1:
                join((x, y), neighbour)

    components: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    for position, cell in by_position.items():
        components[find(position)].append(cell)
    return list(components.values())


def component_features(component: list[dict[str, Any]], all_cells: list[dict[str, Any]]) -> dict[str, float]:
    positions = {visible_cell_key(cell) for cell in component}
    width = max(x for x, _ in positions) - min(x for x, _ in positions) + 8
    height = max(y for _, y in positions) - min(y for _, y in positions) + 8
    bbox_area = max(1, (width // 8) * (height // 8))
    area = len(component)
    compactness = min(1.0, area / bbox_area)
    densities = [density(cell) for cell in component]
    density_range = max(densities) - min(densities) if densities else 0.0
    signatures = Counter(tile_signature(cell) for cell in component)
    repeated = sum(count - 1 for count in signatures.values() if count > 1)
    repetition = min(1.0, repeated / max(1, area - 1))
    perimeter = 0
    for x, y in positions:
        perimeter += sum(
            (x + dx, y + dy) not in positions
            for dx, dy in ((-8, 0), (8, 0), (0, -8), (0, 8))
        )
    boundary = min(1.0, perimeter / max(1, area * 2))
    max_y = max(int(cell.get("screen_y", 0)) for cell in all_cells)
    ground_cells = sum(
        int(cell.get("screen_y", 0)) >= max_y - 16 for cell in component
    )
    ground_contact = min(1.0, ground_cells / max(1, area / 3))
    aspect = width / max(1, height)
    aspect_score = math.exp(-abs(math.log(max(0.25, aspect))))
    return {
        "area": min(1.0, area / 16.0),
        "compactness": compactness,
        "density_variation": min(1.0, density_range * 2.0),
        "repetition": repetition,
        "boundary": boundary,
        "ground_contact": ground_contact,
        "aspect": aspect_score,
    }


def candidate_from_component(
    component: list[dict[str, Any]],
    all_cells: list[dict[str, Any]],
    observations: int,
    frames: list[int],
) -> dict[str, Any]:
    features = component_features(component, all_cells)
    stability = min(1.0, observations / 3.0)
    score = (
        0.22 * features["area"]
        + 0.18 * features["compactness"]
        + 0.14 * features["density_variation"]
        + 0.16 * features["repetition"]
        + 0.12 * features["boundary"]
        + 0.10 * features["ground_contact"]
        + 0.08 * features["aspect"]
        + 0.20 * stability
    )
    positions = [(int(cell.get("map_x", 0)), int(cell.get("map_y", 0)))
                 for cell in component]
    return {
        "class": "background_object_candidate",
        "geometry": "shallow_volume" if score >= 0.65 else "proposal_only",
        "confidence": round(min(1.0, score), 4),
        "observations": observations,
        "frames": frames,
        "map_cells": [
            {"map_x": x, "map_y": y} for x, y in sorted(set(positions))
        ],
        "features": {key: round(value, 4) for key, value in features.items()},
    }


def analyze(records: Iterable[dict[str, Any]]) -> dict[str, Any]:
    frame_candidates: dict[tuple[Any, ...], dict[str, Any]] = {}
    frame_count = 0
    fingerprints: set[int] = set()

    for record in records:
        frame_count += 1
        fingerprints.add(int(record.get("rom_fingerprint", 0)))
        scene = record.get("scene", record)
        sprites = [
            sprite for sprite in scene.get("sprites", []) if sprite.get("visible")
        ]
        cells = [
            cell for cell in scene.get("visible_tile_cells", [])
            if cell.get("source") == "background"
            and not any(overlaps_sprite(cell, sprite) for sprite in sprites)
        ]
        if not cells:
            continue
        for component in union_find_components(cells):
            if len(component) < 2:
                continue
            if len(component) > max(8, int(len(cells) * 0.55)):
                continue
            positions = tuple(sorted(
                (int(cell.get("map_x", 0)), int(cell.get("map_y", 0)),
                 int(cell.get("tile_id", 0)),
                 int(cell.get("attributes", 0)) & 0x78)
                for cell in component
            ))
            entry = frame_candidates.setdefault(
                positions,
                {"count": 0, "frames": [], "component": component,
                 "all_cells": cells},
            )
            entry["count"] += 1
            entry["frames"].append(int(record.get("frame", frame_count - 1)))

    proposals = [
        candidate_from_component(
            entry["component"], entry["all_cells"], entry["count"],
            sorted(set(entry["frames"])),
        )
        for entry in frame_candidates.values()
    ]
    proposals.sort(key=lambda proposal: proposal["confidence"], reverse=True)
    for index, proposal in enumerate(proposals):
        proposal["id"] = f"background-object-{index:04d}"

    return {
        "schema": "gbb.voxel.proposals.v1",
        "frames_analyzed": frame_count,
        "rom_fingerprints": sorted(fingerprints),
        "proposal_policy": {
            "high_confidence": 0.65,
            "medium_confidence": 0.40,
            "low_confidence": "flat",
        },
        "proposals": proposals,
    }


def self_test() -> None:
    def cell(x: int, y: int, tile: int) -> dict[str, Any]:
        return {
            "source": "background",
            "screen_x": x * 8,
            "screen_y": y * 8,
            "visible_width": 8,
            "visible_height": 8,
            "map_x": x,
            "map_y": y,
            "tile_id": tile,
            "attributes": 0,
            "opaque_mask": [255] * 8,
        }

    records = []
    cells = [cell(x, y, 7 if x in (1, 2) and y in (1, 2) else 1)
             for y in range(4) for x in range(4)]
    for frame in range(3):
        records.append({
            "frame": frame,
            "rom_fingerprint": 123,
            "scene": {"visible_tile_cells": cells, "sprites": []},
        })
    result = analyze(records)
    assert result["frames_analyzed"] == 3
    assert result["proposals"]
    assert result["proposals"][0]["observations"] == 3
    assert len(result["proposals"][0]["map_cells"]) == 4


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate pop-up object proposals from scene observation JSONL"
    )
    parser.add_argument("observations", nargs="?", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("voxel scene analyzer self-test passed")
        return 0
    if args.observations is None:
        parser.error("an observation JSONL path is required")

    records = []
    with args.observations.open("r", encoding="utf-8") as input_file:
        for line_number, line in enumerate(input_file, 1):
            if not line.strip():
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise SystemExit(
                    f"{args.observations}:{line_number}: invalid JSON: {error}"
                ) from error
    output = json.dumps(analyze(records), indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
