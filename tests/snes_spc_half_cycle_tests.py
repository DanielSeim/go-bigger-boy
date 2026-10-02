#!/usr/bin/env python3
"""Default-wait-state SPC half clocks; independent processor + timed I/O adapter."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc_bus_cycle_tests import corpus as full_corpus, fixture
from snes_spc_write_cycle_tests import build_reference

EXPECTED = "c8bb3d27569a4e442eb7ee19cab672a1428f881b1170fe14a364119c119f4232"


def corpus():
    for name, data in full_corpus(True):
        lines = []
        for line in data.decode().splitlines():
            fields = line.split()
            if fields[0] in ("host", "host-after"):
                fields[1] = str(int(fields[1]) * 2)
            lines.append(" ".join(fields))
        yield name, ("\n".join(lines) + "\n").encode()
    for port in range(4):
        for half in range(1, 17):
            for after in (False, True):
                data = fixture(bytes((0xba, 0xf4 + port, 0xc4, 0xf4 + port)), 2)
                events = (f"{'host-after' if after else 'host'} {half} {port} 90\n"
                          f"host-after {half} {(port+1)%4} 165\n").encode()
                yield f"split-port{port}-half{half}-after{after}", events + data


def run(binary, data):
    result = subprocess.run([str(binary), "--half-cycles"], input=data,
                            capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError(f"half-clock runner failed: {result.stdout!r} {result.stderr!r}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()
    count = 0
    with tempfile.TemporaryDirectory(prefix="gbb-spc-half-") as directory:
        reference = Path(directory) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir.resolve(), reference)
        for name, data in corpus():
            ours = run(args.gbb.resolve(), data)
            if args.reference_dir and ours != run(reference, data):
                raise AssertionError(f"{name}: independent half-clock trace differs: {ours!r}")
            rows = [line.split() for line in ours.splitlines()]
            for row in rows:
                if row[0] == b"R":
                    early = 0xf4 <= int(row[2]) <= 0xf7
                    if int(row[1]) % 2 != int(early):
                        raise AssertionError("SPC access sampled at the wrong half")
            if int(rows[-1][1]) % 2:
                raise AssertionError("instruction committed before its final half clock")
            digest.update(name.encode() + b"\0" + ours)
            count += 1
        actual = digest.hexdigest()
        print(f"{count} SPC half-cycle cases: {actual}")
        if actual != EXPECTED:
            raise AssertionError("SPC half-clock corpus changed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"SPC half-clock test failed: {error}", file=sys.stderr)
        sys.exit(1)
