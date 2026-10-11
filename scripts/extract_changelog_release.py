#!/usr/bin/env python3
"""Extract the Markdown body of the changelog entry matching a release tag."""

import argparse
from pathlib import Path
import re


def release_notes(changelog: str, tag: str) -> str:
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("Release tag must have the form vX.Y.Z")
    headings = []
    offset = 0
    fence = None
    for line in changelog.splitlines(keepends=True):
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line.rstrip("\r\n"))
        if fence is not None:
            if (marker and marker[1][0] == fence[0]
                    and len(marker[1]) >= len(fence) and not marker[2].strip()):
                fence = None
        elif marker:
            fence = marker[1]
        elif re.match(r"^##(?:\s|$)", line):
            entry = re.match(r"^##\s+\[([^\]]+)\](?:\s.*)?$", line.strip())
            headings.append((offset, offset + len(line), entry[1] if entry else None))
        offset += len(line)

    matches = [i for i, heading in enumerate(headings) if heading[2] == tag[1:]]
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one CHANGELOG.md entry for {tag}; found {len(matches)}")
    index = matches[0]
    start = headings[index][1]
    end = headings[index + 1][0] if index + 1 < len(headings) else len(changelog)
    body = changelog[start:end].strip("\r\n")
    if not body.strip():
        raise ValueError(f"CHANGELOG.md entry for {tag} is empty")
    return body + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("changelog", type=Path)
    parser.add_argument("tag")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        notes = release_notes(args.changelog.read_text(encoding="utf-8"), args.tag)
    except ValueError as error:
        parser.error(str(error))
    args.output.write_text(notes, encoding="utf-8")


if __name__ == "__main__":
    main()
