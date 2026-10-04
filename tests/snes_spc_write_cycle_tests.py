#!/usr/bin/env python3
"""ROM-free SPC700 write boundaries, optionally checked against external bsnes."""

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "98c3ff86ff917d2bf27b9eb8b8921395c98e43a234bda166ca9a4c428f80aa38"


def build_reference(source, output):
    if not (source / "bsnes/processor/spc700/spc700.cpp").is_file():
        raise ValueError("expected a bsnes checkout containing the SPC700 processor")
    subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++17", "-O2", "-isystem", str(source / "bsnes"),
        "-isystem", str(source), "-I", str(ROOT / "tests/support"),
        str(ROOT / "tests/support/snes_spc_reference_write_fixture_runner.cpp"),
        "-o", str(output)], check=True)


def cases():
    # Instruction bytes are original stimuli; offsets include opcode fetch.
    result = {}
    for opcode in (0x0b, 0x2b, 0x4b, 0x6b, 0x8b, 0xab, 0xc4, 0xd8, 0xcb):
        result[f"dp-{opcode:02x}"] = (bytes((opcode, 0x20)), [4])
    for opcode in (0xc6, 0xaf):
        result[f"indirect-{opcode:02x}"] = (bytes((opcode,)), [4])
    for opcode in (0x18, 0x38, 0x98, 0x8f):
        result[f"immediate-{opcode:02x}"] = (bytes((opcode, 0x55, 0x20)), [5])
    for opcode in (0xd4, 0x9b, 0xbb):
        result[f"indexed-{opcode:02x}"] = (bytes((opcode, 0)), [5])
    for opcode in (0xc5, 0xc9, 0xcc, 0x4c, 0x8c):
        result[f"abs-{opcode:02x}"] = (bytes((opcode, 0x20, 0x20)), [5])
    for opcode in (0x0e, 0x4e):
        result[f"testbits-{opcode:02x}"] = (bytes((opcode, 0x20, 0x20)), [6])
    result["or-dp-dp"] = (bytes((0x09, 0x21, 0x20)), [6])
    result["abs-x"] = (bytes((0xd5, 0, 0x20)), [6])
    result["abs-y"] = (bytes((0xd6, 0x19, 0x20)), [6])
    result["pointer-x"] = (bytes((0xc7, 0xf0)), [7])
    result["pointer-y"] = (bytes((0xd7, 0x10)), [7])
    result["dbnz-taken"] = (bytes((0x6e, 0x20, 0)), [4])
    result["dbnz-not-taken"] = (bytes((0x6e, 0x22, 0)), [4])
    result["word-store"] = (bytes((0xda, 0x20)), [4, 5])
    result["word-increment"] = (bytes((0x3a, 0x20)), [4, 6])
    result["word-wrap"] = (bytes((0x3a, 0xff)), [4, 6])
    result["call"] = (bytes((0x3f, 0xc0, 0xff)), [5, 6])
    for opcode in (0x2d, 0x4d, 0x6d):
        result[f"push-{opcode:02x}"] = (bytes((opcode,)), [3])
    for bit in range(8):
        for low in (0x02, 0x12):
            opcode = (bit << 5) | low
            result[f"bit-{opcode:02x}"] = (bytes((opcode, 0x20)), [4])
    return result


def fixture(instruction, page, padding=0):
    setup = bytes((0xe8, 0x5a, 0xcd, 0x20, 0x8d, 7, 0x40 if page else 0x20))
    # Odd padding uses a three-clock read, to cover both key-poll parities.
    extra = bytes(padding // 2) + (bytes((0xe4, 0x23)) if padding % 2 else b"")
    program = setup + extra + instruction
    count = 5 + padding // 2 + padding % 2
    lines = [f"ram {0xffc0+i} {value}" for i, value in enumerate(program)]
    for base in (0, 0x100):
        for address, value in ((0x10, 0x20), (0x11, 0), (0x20, 0x27),
                               (0x21, 0x12), (0x22, 1), (0x23, 0x5a),
                               (0xff, 0xff), (0, 0x12)):
            lines.append(f"ram {base+address} {value}")
    lines += ["ram 8224 39", "ram 39 18", f"run {count}"]
    return ("\n".join(lines) + "\n").encode()


def run(binary, data):
    result = subprocess.run([str(binary)], input=data, capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError(f"runner failed ({result.returncode}): {result.stderr!r}")
    # These are textual traces. Windows' CRT emits CRLF; keep the same
    # independently checked corpus hash and byte comparison on every host.
    return result.stdout.replace(b"\r\n", b"\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="gbb-spc-write-cycle-") as directory:
        reference = Path(directory) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir.resolve(), reference)
        corpus = [(name, instruction, offsets, page, 0)
                  for name, (instruction, offsets) in cases().items() for page in (0, 1)]
        # These short programs stay in the 64-byte synthetic IPL and sweep
        # every modulo-64 write boundary without any external firmware.
        corpus += [(f"boundary-{padding}", bytes((0xc4, 0xf3)), [4], 0, padding)
                   for padding in range(64)]
        for name, instruction, offsets, page, padding in corpus:
            data = fixture(instruction, page, padding)
            ours = run(args.gbb.resolve(), data)
            if args.reference_dir and ours != run(reference, data):
                raise AssertionError(f"{name}/page{page}: independent bus trace differs: {ours!r}")
            writes = [line.split() for line in ours.splitlines() if line.startswith(b"W ")]
            prefix_cycles = 8 + 2*(padding // 2) + 3*(padding % 2)
            if [int(row[1]) - prefix_cycles for row in writes] != offsets:
                raise AssertionError(f"{name}: write offset differs from bus-cycle contract")
            before = [line.split()[1:] for line in ours.splitlines() if line.startswith(b"B ")]
            if before != [row[1:] for row in writes]:
                raise AssertionError("before/after write callbacks disagree")
            digest.update(ours)
        # Keep the same cycle origin across an IPL-to-uploaded-RAM jump.
        handoff = b"ram 65472 143\nram 65473 85\nram 65474 32\n" \
                  b"ram 65475 95\nram 65476 0\nram 65477 4\n" \
                  b"ram 1024 232\nram 1025 90\nram 1026 196\nram 1027 243\nrun 4\n"
        ours = run(args.gbb.resolve(), handoff)
        if args.reference_dir and ours != run(reference, handoff):
            raise AssertionError("IPL-to-RAM handoff differs from independent bus trace")
        writes = [int(line.split()[1]) for line in ours.splitlines()
                  if line.startswith(b"W ")]
        if writes != [5, 14] or not ours.splitlines()[-1].startswith(b"E 14 1028 "):
            raise AssertionError("IPL-to-RAM handoff reset the write clock")
        digest.update(ours)
        actual = digest.hexdigest()
        print(f"{len(corpus) + 1} SPC write-cycle cases: {actual}")
        if actual != EXPECTED:
            raise AssertionError("SPC write-cycle corpus changed")
    return 0


if __name__ == "__main__":
    main()
