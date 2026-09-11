#!/usr/bin/env python3
"""Build a reproducible ROM inventory and a small voxel-analysis corpus.

The ROM directory is intentionally private/ignored. This script writes only
metadata and relative paths; it never copies, renames, or deletes ROMs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


ROM_SUFFIXES = {".gb", ".gbc", ".sgb"}
DEFAULT_CATEGORY_LIMITS = {
    "pokemon_style": 8,
    "platformer": 8,
    "top_down": 6,
    "sgb": 6,
    "homebrew_demo": 8,
    "general": 8,
}


def clean_text(value: bytes) -> str:
    return "".join(chr(byte) if 32 <= byte <= 126 else "?" for byte in value).rstrip()


def filename_quality(name: str) -> int:
    """Prefer a clean dump without rejecting interesting variants."""

    lowered = name.lower()
    score = 0
    if "[!]" in lowered:
        score += 12
    if "(u)" in lowered or "(ue)" in lowered or "(world)" in lowered:
        score += 3
    if "beta" in lowered or "prototype" in lowered:
        score -= 7
    if "translation" in lowered or "t+" in lowered or "romhack" in lowered:
        score -= 5
    if re.search(r"\[(?:a|b|f|h|o|t)\d", lowered):
        score -= 2
    return score


def category_matches(name: str, title: str, sgb: bool) -> list[str]:
    text = f"{name} {title}".lower()
    categories: list[str] = []

    if any(token in text for token in (
        "pokemon", "pokémon", "zelda", "dragon warrior", "dragon quest",
        "final fantasy", "harvest moon", "survival kids", "mystic quest",
        "oracle of", "legend of the river king", "telefang",
    )):
        categories.append("pokemon_style")
    if any(token in text for token in (
        "mario", "kirby", "donkey kong", "wario", "adventure island",
        "castlevania", "megaman", "mega man", "contra", "metroid",
        "rayman", "turok", "ducktales", "batman", "spider-man",
        "spiderman", "ninja", "shantae", "trip world",
    )):
        categories.append("platformer")
    if any(token in text for token in (
        "tetris", "puzzle", "boxxle", "boulder", "gauntlet", "gargoyle",
        "final fantasy", "dragon warrior", "zelda", "pokemon", "harvest",
        "golf", "baseball", "soccer", "fifa", "pinball",
    )):
        categories.append("top_down")
    if sgb or "sgb" in text or "super game boy" in text:
        categories.append("sgb")
    if any(token in text for token in (
        "(pd)", "homebrew", "gambatte", "acid2", "dmg-acid",
        "darkfader", "sameboy", "cgb-acid",
    )) or re.search(r"\bdemos?\b", text):
        categories.append("homebrew_demo")
    if not categories:
        categories.append("general")
    return categories


def inspect_rom(path: Path, root: Path) -> dict[str, Any]:
    data = path.read_bytes()
    relative = path.relative_to(root).as_posix()
    title = clean_text(data[0x134:0x143]) if len(data) >= 0x143 else ""
    cgb_flag = data[0x143] if len(data) > 0x143 else None
    sgb_flag = data[0x146] if len(data) > 0x146 else None
    cartridge_type = data[0x147] if len(data) > 0x147 else None
    rom_size_code = data[0x148] if len(data) > 0x148 else None
    ram_size_code = data[0x149] if len(data) > 0x149 else None
    sgb = sgb_flag == 0x03
    display_title = title or path.stem
    categories = category_matches(path.name, display_title, sgb)
    return {
        "path": relative,
        "filename": path.name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "title": display_title,
        "platform": "gbc" if cgb_flag is not None and cgb_flag & 0x80 else "gb",
        "sgb": sgb,
        "cartridge_type": cartridge_type,
        "rom_size_code": rom_size_code,
        "ram_size_code": ram_size_code,
        "categories": categories,
        "filename_quality": filename_quality(path.name),
    }


def deduplicate(entries: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], int]:
    by_hash: dict[str, dict[str, Any]] = {}
    duplicates = 0
    for entry in entries:
        previous = by_hash.get(entry["sha256"])
        if previous is None:
            by_hash[entry["sha256"]] = entry
            continue
        duplicates += 1
        if (entry["filename_quality"], entry["path"]) > (
            previous["filename_quality"], previous["path"]
        ):
            by_hash[entry["sha256"]] = entry
    unique = list(by_hash.values())
    unique.sort(key=lambda entry: entry["path"].casefold())
    return unique, duplicates


def select_corpus(
    entries: list[dict[str, Any]], limits: dict[str, int]
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    selected: dict[str, dict[str, Any]] = {}
    selected_categories: dict[str, int] = defaultdict(int)

    def ranking(entry: dict[str, Any]) -> tuple[Any, ...]:
        return (
            -entry["filename_quality"],
            -entry["size"],
            entry["path"].casefold(),
        )

    # An entry may satisfy multiple passes but is only emitted once.
    for category in limits:
        candidates = sorted(
            (entry for entry in entries if category in entry["categories"]),
            key=ranking,
        )
        for entry in candidates:
            if selected_categories[category] >= limits[category]:
                break
            selected.setdefault(entry["sha256"], entry)
            selected_categories[category] += 1

    corpus = list(selected.values())
    corpus.sort(key=lambda entry: (entry["categories"][0], entry["path"].casefold()))
    actual_categories = {
        category: sum(category in entry["categories"] for entry in corpus)
        for category in limits
    }
    return corpus, actual_categories


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-dir", type=Path, default=Path("roms"))
    parser.add_argument(
        "--manifest", type=Path, default=Path("roms/voxel-rom-manifest.json")
    )
    parser.add_argument(
        "--corpus", type=Path, default=Path("roms/voxel-observation-corpus.json")
    )
    parser.add_argument(
        "--limit", type=int, default=None,
        help="override every category limit",
    )
    args = parser.parse_args()

    if not args.rom_dir.is_dir():
        print(f"ROM directory does not exist: {args.rom_dir}", file=sys.stderr)
        return 2
    paths = sorted(
        (
            path for path in args.rom_dir.rglob("*")
            if path.is_file() and path.suffix.lower() in ROM_SUFFIXES
        ),
        key=lambda path: path.as_posix().casefold(),
    )
    if not paths:
        print(f"No Game Boy ROMs found below {args.rom_dir}", file=sys.stderr)
        return 2

    entries: list[dict[str, Any]] = []
    invalid = 0
    for path in paths:
        try:
            entry = inspect_rom(path, args.rom_dir)
        except OSError as error:
            print(f"warning: could not read {path}: {error}", file=sys.stderr)
            invalid += 1
            continue
        entry["valid_header_size"] = entry["size"] >= 0x150
        if not entry["valid_header_size"]:
            invalid += 1
        entries.append(entry)

    unique, duplicate_count = deduplicate(entries)
    limits = dict(DEFAULT_CATEGORY_LIMITS)
    if args.limit is not None:
        if args.limit < 1:
            print("--limit must be positive", file=sys.stderr)
            return 2
        limits = {category: args.limit for category in limits}
    corpus, category_counts = select_corpus(unique, limits)

    common = {
        "schema": "gbb.voxel.rom-manifest.v1",
        "rom_directory": args.rom_dir.as_posix(),
        "extensions": sorted(ROM_SUFFIXES),
        "source_files": len(paths),
        "unique_files": len(unique),
        "exact_duplicates_removed": duplicate_count,
        "short_or_unreadable_files": invalid,
    }
    write_json(args.manifest, {**common, "entries": unique})
    write_json(
        args.corpus,
        {
            **common,
            "selection_policy": {
                "limits": limits,
                "deduplication": "exact SHA-256 only; revisions, translations, hacks, and betas remain distinct",
                "selection": "prefer clean verified-looking filenames, then larger ROMs, then stable path order",
            },
            "category_counts": category_counts,
            "entries": corpus,
        },
    )

    print(f"Scanned {len(paths)} ROM files")
    print(f"Unique content files: {len(unique)} ({duplicate_count} exact duplicates removed)")
    print(f"Curated observation corpus: {len(corpus)} entries")
    for category, count in category_counts.items():
        print(f"  {category}: {count}")
    print(f"Manifest: {args.manifest}")
    print(f"Corpus: {args.corpus}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
