#!/usr/bin/env python3
"""Convert a GitHub Release description to Google Play update news."""

import argparse
import html
import json
from pathlib import Path
import re


def play_release_notes(body: str, release_url: str) -> str:
    text = re.sub(r"<!--.*?-->", "", body, flags=re.DOTALL)
    text = re.sub(r"!\[([^\]]*)\]\([^\n)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^\n)]*\)", r"\1", text)
    text = re.sub(r"(?m)^\s{0,3}#{1,6}\s+", "", text)
    text = re.sub(r"(?m)^\s*```[^\n]*$", "", text)
    text = text.replace("**", "").replace("__", "").replace("`", "")
    text = html.unescape(text).replace("\r\n", "\n")
    text = "\n".join(line.strip() for line in text.splitlines())
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    if not text:
        raise ValueError("GitHub Release description must not be empty")

    # Count UTF-16 units conservatively, including supplementary characters.
    def length(value: str) -> int:
        return len(value.encode("utf-16-le")) // 2

    if length(text) <= 500:
        return text
    suffix = f"…\n\nFull release notes: {release_url}"
    budget = 500 - length(suffix)
    if budget < 1:
        raise ValueError("Release URL leaves no room for update news")
    excerpt = []
    used = 0
    for character in text:
        used += length(character)
        if used > budget:
            break
        excerpt.append(character)
    return "".join(excerpt).rstrip() + suffix


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_json", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    release = json.loads(args.release_json.read_text(encoding="utf-8"))
    notes = play_release_notes(release.get("body") or "", release["html_url"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(notes, encoding="utf-8")
    print(notes)


if __name__ == "__main__":
    main()
