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

MAX_TEMPLATE_PROPOSALS = 32


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


def contextual_boundary(component: list[dict[str, Any]],
                        all_cells: list[dict[str, Any]]) -> float:
    """Measure how strongly a candidate's outside edge differs from it."""

    component_positions = {visible_cell_key(cell) for cell in component}
    by_position = {visible_cell_key(cell): cell for cell in all_cells}
    exposed = 0
    contrasting = 0
    for x, y in component_positions:
        for neighbour in ((x - 8, y), (x + 8, y), (x, y - 8), (x, y + 8)):
            if neighbour in component_positions:
                continue
            outside = by_position.get(neighbour)
            if outside is None:
                continue
            exposed += 1
            inside = by_position[(x, y)]
            if (int(inside.get("tile_id", 0)),
                int(inside.get("attributes", 0)) & 0x78) != (
                    int(outside.get("tile_id", 0)),
                    int(outside.get("attributes", 0)) & 0x78):
                contrasting += 1
    return contrasting / max(1, exposed)


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
        "boundary_contrast": contextual_boundary(component, all_cells),
        "ground_contact": ground_contact,
        "aspect": aspect_score,
    }


def component_layout(component: list[dict[str, Any]]) -> dict[str, Any]:
    """Return a scroll-stable tile layout and a profile-template candidate."""

    screen_positions = [
        (int(cell.get("screen_x", 0)) // 8,
         int(cell.get("screen_y", 0)) // 8)
        for cell in component
    ]
    map_addresses = [int(cell.get("map_address", 0)) for cell in component]
    # Map addresses remain stable while the viewport scrolls and avoid the
    # 8-bit map_x/map_y wrap at the edge of a tilemap. Use screen coordinates
    # for the relative rectangle itself so wrapped neighbors stay adjacent.
    # Missing map provenance commonly serializes every address as zero; mark
    # that fallback as less trustworthy for the authoring score.
    map_usable = (len(component) > 1 and len(set(map_addresses)) == len(component)
                  and any(map_addresses))
    coordinates = screen_positions
    coordinate_mode = "map" if map_usable else "screen"
    min_x = min(x for x, _ in coordinates)
    min_y = min(y for _, y in coordinates)
    relative = [(x - min_x, y - min_y) for x, y in coordinates]
    width = max(x for x, _ in relative) + 1
    height = max(y for _, y in relative) + 1
    complete = len(set(relative)) == width * height
    tile_by_relative = {
        position: int(cell.get("tile_id", 0))
        for position, cell in zip(relative, component)
    }
    tile_ids = (
        [tile_by_relative[(x, y)]
         for y in range(height) for x in range(width)]
        if complete else []
    )
    return {
        "coordinate_mode": coordinate_mode,
        "anchor": ({"map_address": min(map_addresses)} if map_usable else
                   {"screen_x": min_x, "screen_y": min_y}),
        "width": width,
        "height": height,
        "complete": complete,
        "relative_cells": [
            {"x": x, "y": y, "tile_id": tile_by_relative[(x, y)]}
            for x, y in sorted(relative, key=lambda position: (position[1], position[0]))
        ],
        "tile_ids": tile_ids,
    }


def candidate_identity(component: list[dict[str, Any]]) -> tuple[Any, ...]:
    layout = component_layout(component)
    coordinates = [
        (int(cell.get("map_address", 0)), 0)
        for cell in component
    ] if layout["coordinate_mode"] == "map" else [
        (int(cell.get("screen_x", 0)) // 8,
         int(cell.get("screen_y", 0)) // 8)
        for cell in component
    ]
    return tuple(sorted(
        (x, y, int(cell.get("tile_id", 0)),
         int(cell.get("attributes", 0)) & 0x78)
        for (x, y), cell in zip(coordinates, component)
    ))


def rectangular_layout_candidates(cells: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    """Find small complete layouts even when neighboring tile IDs differ."""

    by_screen = {
        visible_cell_key(cell): cell
        for cell in cells
        if int(cell.get("visible_width", 8)) == 8
        and int(cell.get("visible_height", 8)) == 8
    }
    candidates: list[list[dict[str, Any]]] = []
    # Buildings commonly fit these metatile-sized footprints. Larger or
    # irregular arrangements should be authored explicitly after review.
    for width, height in ((2, 2), (2, 3), (3, 2), (3, 3)):
        for left, top in sorted(by_screen):
            component: list[dict[str, Any]] = []
            for row in range(height):
                for column in range(width):
                    cell = by_screen.get((left + column * 8, top + row * 8))
                    if cell is None:
                        component = []
                        break
                    component.append(cell)
                if not component:
                    break
            if not component:
                continue
            if len({int(cell.get("tile_id", 0)) for cell in component}) < 2:
                continue
            candidates.append(component)
    return candidates


def candidate_from_component(
    component: list[dict[str, Any]],
    all_cells: list[dict[str, Any]],
    observations: int,
    frames: list[int],
) -> dict[str, Any]:
    features = component_features(component, all_cells)
    layout = component_layout(component)
    stability = min(1.0, observations / 3.0)
    score = (
        0.22 * features["area"]
        + 0.18 * features["compactness"]
        + 0.14 * features["density_variation"]
        + 0.16 * features["repetition"]
        + 0.08 * features["boundary"]
        + 0.04 * features["boundary_contrast"]
        + 0.10 * features["ground_contact"]
        + 0.08 * features["aspect"]
        + 0.20 * stability
    )
    positions = [(int(cell.get("map_x", 0)), int(cell.get("map_y", 0)))
                 for cell in component]
    candidate = {
        "class": "background_object_candidate",
        "geometry": "shallow_volume" if score >= 0.65 else "proposal_only",
        "confidence": round(min(1.0, score), 4),
        "observations": observations,
        "frames": frames,
        "map_cells": [
            {"map_x": x, "map_y": y} for x, y in sorted(set(positions))
        ],
        "features": {key: round(value, 4) for key, value in features.items()},
        "coordinate_mode": layout["coordinate_mode"],
        "anchor": layout["anchor"],
        "relative_cells": layout["relative_cells"],
    }
    if layout["complete"]:
        candidate["template"] = {
            "width": layout["width"],
            "height": layout["height"],
            "tile_ids": layout["tile_ids"],
        }
    return candidate


def template_proposals(proposals: list[dict[str, Any]],
                       frames_analyzed: int,
                       minimum_capture_frames: int = 30) -> list[dict[str, Any]]:
    """Cluster recurring complete layouts into manual-review profile rules."""

    if frames_analyzed < minimum_capture_frames:
        return []

    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for proposal in proposals:
        template = proposal.get("template")
        if not template:
            continue
        key = (
            proposal.get("coordinate_mode", "screen"),
            int(template["width"]),
            int(template["height"]),
            tuple(int(tile) for tile in template["tile_ids"]),
        )
        groups[key].append(proposal)

    output: list[dict[str, Any]] = []
    for index, (key, members) in enumerate(groups.items()):
        coordinate_mode, width, height, tile_ids = key
        tile_counts = Counter(tile_ids)
        dominant_fraction = max(tile_counts.values()) / max(1, len(tile_ids))
        # A rectangle made almost entirely from one common tile is usually a
        # sliding window through terrain, not a semantic object.
        if dominant_fraction > 0.75:
            continue
        anchors = {
            tuple(sorted(proposal.get("anchor", {}).items()))
            for proposal in members
        }
        frames = sorted({
            int(frame)
            for proposal in members
            for frame in proposal.get("frames", [])
        })
        observations = sum(int(proposal.get("observations", 0))
                          for proposal in members)
        mean_candidate_confidence = sum(
            float(proposal.get("confidence", 0.0)) for proposal in members
        ) / max(1, len(members))
        mean_ground_contact = sum(
            float(proposal.get("features", {}).get("ground_contact", 0.0))
            for proposal in members
        ) / max(1, len(members))
        mean_boundary_contrast = sum(
            float(proposal.get("features", {}).get("boundary_contrast", 0.0))
            for proposal in members
        ) / max(1, len(members))
        recurrence = min(1.0, observations / 6.0)
        location_score = min(1.0, len(anchors) / 2.0)
        risk = 0.0
        risk_reasons: list[str] = []
        if coordinate_mode != "map":
            risk += 0.25
            risk_reasons.append("map provenance is unavailable")
        if len(anchors) == 1:
            risk += 0.10
            risk_reasons.append("seen at one location")
        if width * height <= 2:
            risk += 0.12
            risk_reasons.append("small tile arrangement")
        if mean_ground_contact < 0.35:
            risk += 0.10
            risk_reasons.append("weak ground contact")
        if mean_boundary_contrast < 0.55:
            risk += 0.25
            risk_reasons.append("weak contextual boundary")
        if dominant_fraction > 0.65:
            risk += 0.15
            risk_reasons.append("one tile dominates the footprint")
        if len(anchors) > 8:
            risk += 0.25
            risk_reasons.append("pattern repeats across many locations")
        if mean_boundary_contrast < 0.55:
            continue
        if len(anchors) > 8 and mean_boundary_contrast < 0.75:
            continue
        footprint_score = min(1.0, 4.0 / max(1, width * height))
        base_confidence = (
            0.35 * mean_candidate_confidence
            + 0.20 * recurrence
            + 0.15 * location_score
            + 0.15 * mean_ground_contact
            + 0.10 * mean_boundary_contrast
            + 0.05 * footprint_score
        )
        confidence = round(max(0.0, min(1.0, base_confidence - risk)), 4)
        tile_text = ",".join(str(tile) for tile in tile_ids)
        output.append({
            "id": f"template-proposal-{index:04d}",
            "class": "background_object_template_proposal",
            "decision": "manual_review",
            "confidence": confidence,
            "geometry": "shallow_volume" if confidence >= 0.65 else "proposal_only",
            "coordinate_mode": coordinate_mode,
            "template": {
                "width": width,
                "height": height,
                "tile_ids": list(tile_ids),
            },
            "profile_entry": (
                f"background_object_template={width}x{height}:{tile_text}"
            ),
            "observations": observations,
            "frames": frames,
            "locations": len(anchors),
            "source_proposals": [proposal.get("id", "") for proposal in members],
            "evidence": {
                "candidate_confidence": round(mean_candidate_confidence, 4),
                "recurrence": round(recurrence, 4),
                "location_score": round(location_score, 4),
                "ground_contact": round(mean_ground_contact, 4),
                "boundary_contrast": round(mean_boundary_contrast, 4),
                "dominant_tile_fraction": round(dominant_fraction, 4),
                "risk": round(min(1.0, risk), 4),
            },
            "risk_reasons": risk_reasons,
        })
    output.sort(key=lambda proposal: proposal["confidence"], reverse=True)
    # Keep the report a ranked shortlist. The full component proposals remain
    # available for diagnostics, while hundreds of overlapping windows should
    # not obscure the few candidates worth human review.
    output = output[:MAX_TEMPLATE_PROPOSALS]
    for index, proposal in enumerate(output):
        proposal["id"] = f"template-proposal-{index:04d}"
    return output


def analyze(records: Iterable[dict[str, Any]],
            minimum_capture_frames: int = 30) -> dict[str, Any]:
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
            positions = candidate_identity(component)
            entry = frame_candidates.setdefault(
                positions,
                {"count": 0, "frames": [], "component": component,
                 "all_cells": cells},
            )
            entry["count"] += 1
            entry["frames"].append(int(record.get("frame", frame_count - 1)))

        # The graph deliberately remains conservative for live rendering, but
        # authoring can inspect complete metatile footprints with unrelated
        # tile IDs. These are proposal-only until recurrence and risk scoring
        # (and ultimately human review) accepts them.
        for component in rectangular_layout_candidates(cells):
            positions = candidate_identity(component)
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

    template_candidates = template_proposals(
        proposals, frame_count, minimum_capture_frames
    )
    return {
        "schema": "gbb.voxel.proposals.v2",
        "frames_analyzed": frame_count,
        "capture_quality": {
            "eligible_for_templates": frame_count >= minimum_capture_frames,
            "minimum_template_frames": minimum_capture_frames,
        },
        "rom_fingerprints": sorted(fingerprints),
        "proposal_policy": {
            "high_confidence": 0.65,
            "medium_confidence": 0.40,
            "low_confidence": "flat",
        },
        "proposals": proposals,
        "template_proposals": template_candidates,
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
    assert any(
        len(proposal["map_cells"]) == 4
        and proposal.get("template", {}).get("tile_ids") == [7, 7, 7, 7]
        for proposal in result["proposals"]
    )
    assert not result["template_proposals"]


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
