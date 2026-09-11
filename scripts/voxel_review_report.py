#!/usr/bin/env python3
"""Render a self-contained HTML/SVG review report for voxel proposals."""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path
from typing import Any


def resolve_path(value: str, manifest: Path) -> Path:
    path = Path(value)
    if path.is_absolute() or path.exists():
        return path
    return manifest.parent / path


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    records = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as error:
            raise ValueError(
                f"{path}:{line_number}: invalid JSON: {error}"
            ) from error
    return records


def proposal_color(confidence: float) -> str:
    if confidence >= 0.65:
        return "#00c2ff"
    if confidence >= 0.40:
        return "#ffb000"
    return "#ff4d6d"


def frame_choices(
    records: list[dict[str, Any]], proposals: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    if not records:
        return []
    desired = {0, len(records) - 1, len(records) // 3, (len(records) * 2) // 3}
    for proposal in proposals[:8]:
        desired.update(int(frame) for frame in proposal.get("frames", [])[:1])
    return [
        records[index]
        for index in sorted(desired)
        if 0 <= index < len(records)
    ][:8]


def render_frame(
    record: dict[str, Any], proposals: list[dict[str, Any]]
) -> str:
    scene = record.get("scene", record)
    width = int(scene.get("width", 160))
    height = int(scene.get("height", 144))
    by_map: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for proposal in proposals:
        confidence = float(proposal.get("confidence", 0.0))
        for cell in proposal.get("map_cells", []):
            key = (int(cell.get("map_x", 0)), int(cell.get("map_y", 0)))
            by_map.setdefault(key, []).append({
                "proposal": proposal,
                "confidence": confidence,
            })

    parts = [
        f'<svg class="scene" viewBox="0 0 {width} {height}" role="img" '
        f'aria-label="Frame {html.escape(str(record.get("frame", "?")))}">',
        '<rect width="100%" height="100%" fill="#101827"/>',
    ]
    for cell in scene.get("visible_tile_cells", []):
        x = int(cell.get("screen_x", 0))
        y = int(cell.get("screen_y", 0))
        cell_width = int(cell.get("visible_width", 8))
        cell_height = int(cell.get("visible_height", 8))
        source = cell.get("source", "background")
        base = "#40516a" if source == "background" else "#73556c"
        map_key = (int(cell.get("map_x", 0)), int(cell.get("map_y", 0)))
        matches = by_map.get(map_key, [])
        if matches:
            match = max(matches, key=lambda item: item["confidence"])
            proposal = match["proposal"]
            confidence = match["confidence"]
            base = proposal_color(confidence)
            label = (
                f'{proposal.get("id", "proposal")} '
                f'confidence={confidence:.2f} map={map_key}'
            )
        else:
            label = f'{source} map={map_key}'
        parts.append(
            f'<rect x="{x}" y="{y}" width="{cell_width}" '
            f'height="{cell_height}" fill="{base}" '
            f'fill-opacity="{0.78 if matches else 0.35}" '
            'stroke="#dbeafe" stroke-opacity="0.16" stroke-width="0.35">'
            f'<title>{html.escape(label)}</title></rect>'
        )
    for sprite in scene.get("sprites", []):
        if not sprite.get("visible"):
            continue
        x = int(sprite.get("screen_x", 0))
        y = int(sprite.get("screen_y", 0))
        sprite_height = 16 if int(sprite.get("attributes", 0)) & 0x04 else 8
        parts.append(
            f'<rect x="{x}" y="{y}" width="8" height="{sprite_height}" '
            'fill="none" stroke="#ff4d6d" stroke-width="1">'
            f'<title>visible sprite tile={int(sprite.get("tile", 0))}</title></rect>'
        )
    input_buttons = html.escape(str(record.get("input_buttons", "none")))
    frame = html.escape(str(record.get("frame", "?")))
    parts.append(
        f'<text x="3" y="10" fill="#ffffff" font-size="5" '
        f'font-family="monospace">frame {frame} · {input_buttons}</text>'
    )
    parts.append("</svg>")
    return "".join(parts)


def render_run(result: dict[str, Any], manifest: Path) -> str:
    proposal_path = resolve_path(result.get("proposals", ""), manifest)
    observation_path = resolve_path(result.get("observations", ""), manifest)
    status = result.get("status", "unknown")
    title = html.escape(str(result.get("title") or result.get("path", "ROM")))
    if status not in {"complete", "partial_capture", "skipped"} or not proposal_path.is_file() or not observation_path.is_file():
        return (
            '<article class="run failed"><h2>' + title + '</h2>'
            f'<p>Status: <code>{html.escape(str(status))}</code></p>'
            f'<p>{html.escape(str(result.get("path", "")))}</p></article>'
        )
    proposals = json.loads(
        proposal_path.read_text(encoding="utf-8")
    ).get("proposals", [])
    records = read_jsonl(observation_path)
    confidence = max(
        (float(item.get("confidence", 0.0)) for item in proposals),
        default=0.0,
    )
    frames = "".join(
        f'<figure>{render_frame(record, proposals)}'
        f'<figcaption>Frame {html.escape(str(record.get("frame", "?")))}</figcaption></figure>'
        for record in frame_choices(records, proposals)
    )
    proposal_rows = "".join(
        "<tr>"
        f"<td>{html.escape(str(proposal.get('id', '')))}</td>"
        f"<td>{float(proposal.get('confidence', 0.0)):.2f}</td>"
        f"<td>{html.escape(str(proposal.get('geometry', '')))}</td>"
        f"<td>{int(proposal.get('observations', 0))}</td>"
        "</tr>"
        for proposal in proposals[:20]
    )
    status_label = {
        "complete": "complete",
        "partial_capture": "partial capture",
        "skipped": "reused existing capture",
    }[status]
    return (
        f'<article class="run"><h2>{title}</h2>'
        f'<p>Status: <code>{status_label}</code></p>'
        f'<p><code>{html.escape(str(result.get("path", "")))}</code> · '
        f'{len(records)} frames · {len(proposals)} proposals · '
        f'max confidence {confidence:.2f}</p>'
        f'<div class="frames">{frames}</div>'
        '<details><summary>Proposal table</summary><table>'
        '<thead><tr><th>ID</th><th>Confidence</th><th>Geometry</th><th>Frames</th></tr></thead>'
        f'<tbody>{proposal_rows}</tbody></table></details></article>'
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_manifest", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.run_manifest.is_file():
        parser.error(f"run manifest does not exist: {args.run_manifest}")
    output = args.output or args.run_manifest.parent / "index.html"
    data = json.loads(args.run_manifest.read_text(encoding="utf-8"))
    manifest_path = args.run_manifest.resolve()
    results = data.get("results", [])
    articles = "\n".join(render_run(result, manifest_path) for result in results)
    complete = sum(result.get("status") == "complete" for result in results)
    partial = sum(result.get("status") == "partial_capture" for result in results)
    document = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Go Bigger Boy voxel proposal review</title>
<style>
body {{ margin: 0; background: #0b1120; color: #dbeafe; font: 15px system-ui, sans-serif; }}
main {{ max-width: 1500px; margin: 0 auto; padding: 24px; }}
h1 {{ margin-top: 0; }}
.run {{ margin: 24px 0; padding: 18px; border: 1px solid #263a57; border-radius: 12px; background: #111c30; }}
.failed {{ border-color: #7f1d1d; }}
.frames {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 14px; }}
figure {{ margin: 0; padding: 8px; background: #0b1120; border-radius: 8px; }}
.scene {{ display: block; width: 100%; image-rendering: pixelated; }}
figcaption {{ color: #93c5fd; font-family: monospace; margin-top: 6px; }}
code {{ color: #7dd3fc; }}
table {{ border-collapse: collapse; margin-top: 12px; }}
th, td {{ border-bottom: 1px solid #263a57; padding: 5px 10px; text-align: left; }}
summary {{ cursor: pointer; color: #38bdf8; }}
</style></head><body><main>
<h1>Voxel proposal review</h1>
<p>{complete}/{len(results)} ROM runs completed, {partial} partial captures.
This report is proposal-only; no geometry was accepted into live rendering.</p>
{articles}
</main></body></html>
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(document, encoding="utf-8")
    print(f"Wrote review report: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
