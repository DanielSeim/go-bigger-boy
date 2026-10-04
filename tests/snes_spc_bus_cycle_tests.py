#!/usr/bin/env python3
"""ROM-free SPC bus traces and analytical timer/port boundary comparisons."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc_write_cycle_tests import build_reference, cases as write_cases, fixture as write_fixture

EXPECTED = "19614ed37f3b3f847935a61b4416a8dffc6774b3fe028efe67804fe27f08b1e2"
RESUMABLE_EXPECTED = "d7f8dade8aa3c7c5beb331ec9aaaa9232e72cf9fe4c8406b9c8536cec528dd39"
OPCODES = bytes.fromhex("""
00 02 03 04 08 09 0b 0e 10 12 13 18 1c 1d 1f 20 22 23 24 28 2b 2d 2f
30 32 33 38 3a 3d 3f 40 42 43 44 48 4b 4c 4d 4e 52 53 5c 5d 5e 5f 60
62 63 64 68 69 6b 6d 6e 6f 72 73 75 78 7a 7c 7d 7e 80 82 83 84 85 88
8b 8c 8d 8f 90 92 93 95 96 98 9a 9b 9e 9f a2 a3 a8 ab ad ae af b0 b2
b3 b5 b6 ba bb bc bd c2 c3 c4 c5 c6 c7 c8 c9 cb cc cd ce cf d0 d2 d3 d4
d5 d6 d7 d8 da dc dd de e2 e3 e4 e5 e6 e7 e8 e9 eb ec ed ee f0 f2 f3 f4
f5 f6 f7 f8 fb fc fd fe
""")


def fixture(ipl, count, ram=(), hosts=()):
    assert len(ipl) <= 64
    lines = [f"ram {0xffc0+i} {value}" for i, value in enumerate(ipl)]
    lines += [f"ram {address} {value}" for address, value in ram]
    lines += [f"host {cycle} {port} {value}" for cycle, port, value in hosts]
    lines.append(f"run {count}")
    return ("\n".join(lines) + "\n").encode()


def padded(instruction, timer, target, padding, hosts=()):
    # Original test IPL sets the timer then jumps to original RAM code.
    ipl = bytes((0x8f, target, 0xfa+timer, 0x8f, 1 << timer, 0xf1,
                 0x5f, 0, 4))
    wait = bytes(padding // 2) + (bytes((0xe4, 0x20)) if padding % 2 else b"")
    code = wait + instruction
    ram = [(0x400+i, value) for i, value in enumerate(code)]
    return fixture(ipl, 4 + padding // 2 + padding % 2, ram, hosts)


def corpus(resumable=False):
    for page in (0, 1):
        for memory in (0, 255):
            for opcode in OPCODES:
                program = bytes((0xe8, 0x5a, 0xcd, 0x20, 0x8d, 7,
                                 0x40 if page else 0x20, opcode, 0x20, 0x20, 0))
                ram = [(address, memory) for address in (0x20, 0x21, 0x120, 0x121,
                                                         0x40, 0x41, 0x140, 0x141,
                                                         0x2020, 0x2040, 0x2027)]
                yield f"opcode-{opcode:02x}-page{page}-memory{memory}", fixture(program, 5, ram)
    for name, (instruction, _) in write_cases().items():
        for page in (0, 1):
            yield f"write-{name}-page{page}", write_fixture(instruction, page)
    for padding in range(64):
        yield f"key-boundary-{padding}", write_fixture(bytes((0xc4, 0xf3)), 0, padding)
    for opcode in (0xd0, 0xf0, 0x10, 0x30, 0xb0, 0x90):
        for value in (0, 0x80, 0x5a):
            for carry in (0, 1):
                yield f"branch-{opcode:02x}-a{value}-c{carry}", fixture(
                    bytes((0xe8, value, 0x80 if carry else 0x60, opcode, 0)), 3)
    yield "dbnz-y-not-taken", fixture(bytes((0x8d, 1, 0xfe, 0)), 2)
    yield "timer-control-reenable", fixture(
        bytes((0x8f, 1, 0xfc, 0x8f, 4, 0xf1, 0, 0, 0, 0, 0, 0,
               0x8f, 0, 0xf1, 0, 0, 0x8f, 4, 0xf1, 0xe4, 0xff)), 14)
    # Covers each timer's entire stage-1 period, destructive reads and stores'
    # dummy reads. The reference processor uses a separate one-clock oracle.
    for timer in range(3):
        for padding in range(16 if timer == 2 else 128):
            for opcode in (0xe4, 0xeb, 0xf8, 0x0b, 0xc4, 0xda, 0x3a):
                yield f"timer{timer}-{padding}-{opcode:02x}", padded(
                    bytes((opcode, 0xfd+timer)), timer, 1, padding)
        for target in (0, 2, 255):
            for padding in (0, 15, 127, 255, 4095, 4096):
                yield f"target-{timer}-{target}-{padding}", padded(
                    bytes((0xe4, 0xfd+timer)), timer, target, padding)
    for padding in range(16):
        for opcode in (0x0e, 0x4e):
            yield f"double-timer-read-{padding}-{opcode:02x}", padded(
                bytes((opcode, 0xff, 0)), 2, 1, padding)
    # Host writes occur at stated completed clock boundaries, before accesses.
    # This is deterministic port ordering, not a full SNES rendezvous model.
    for port in range(4):
        for cycle in range(14, 20):
            yield f"port-{port}-clock{cycle}", padded(
                bytes((0xba, 0xf4+port)), 2, 1, 0,
                [(cycle, port, 0x5a), (cycle+1, (port+1) % 4, 0xa5)])
    # Timer output itself supplies an opcode (NOP), then the implied dummy
    # fetch clears the adjacent timer register. No code bytes are at $FC.
    yield "dummy-fetch-io", fixture(bytes((0x8f, 1, 0xfa, 0x8f, 1, 0xf1,
                                            0x5f, 0xfc, 0)), 4)
    # A store's dummy read clears timer 2; a following explicit read sees zero.
    yield "store-clears-timer", fixture(
        bytes((0x8f, 1, 0xfc, 0x8f, 4, 0xf1, 0, 0, 0xc4, 0xff, 0xe4, 0xff)), 6)
    if resumable:
        # Host events before/after the same completed bus access distinguish
        # ties, suspended word reads, and port clearing followed by late input.
        for opcode in (0xe4, 0xba, 0xc4, 0xda, 0x0b, 0x8f):
            for port in range(4):
                for cycle in range(1, 11):
                    program = (bytes((opcode, 0x30, 0xf1)) if opcode == 0x8f
                               else bytes((opcode, 0xf4+port))) + bytes((0xe4, 0xf4+port))
                    data = fixture(program, 2)
                    events = (f"host {cycle} {port} 90\n"
                              f"host-after {cycle} {port} 165\n"
                              f"host-after {cycle} {(port+1)%4} 51\n").encode()
                    yield f"rendezvous-{opcode:02x}-port{port}-clock{cycle}", events + data
        for cycle in range(1, 12):
            data = fixture(bytes((0x8f, 0x30, 0xf1, 0xba, 0xf4, 0xba, 0xf6)), 3)
            events = b"".join(f"host-after {cycle} {port} {port+81}\n".encode()
                              for port in range(4))
            yield f"rendezvous-port-clear-{cycle}", events + data


def run(binary, data, resumable=False):
    result = subprocess.run([str(binary), "--resumable-cycles" if resumable else "--bus-cycles"], input=data,
                            capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError(f"bus runner failed: {result.returncode}: {result.stdout!r}")
    return result.stdout.replace(b"\r\n", b"\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    parser.add_argument("--resumable", action="store_true")
    args = parser.parse_args()
    digest = hashlib.sha256()
    count = 0
    with tempfile.TemporaryDirectory(prefix="gbb-spc-bus-") as directory:
        reference = Path(directory) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir.resolve(), reference)
        for name, data in corpus(args.resumable):
            ours = run(args.gbb.resolve(), data, args.resumable)
            if args.reference_dir:
                expected = run(reference, data)
                if ours != expected:
                    raise AssertionError(f"{name}: bus traces differ\nGBB: {ours!r}\nREF: {expected!r}")
            # Exactly one accepted read/write/idle at every completed clock.
            rows = [row.split() for row in ours.splitlines()]
            clocks = [int(row[1]) for row in rows if row[0] in (b"R", b"W", b"I")]
            if clocks != list(range(1, int(rows[-1][1])+1)):
                raise AssertionError(f"{name}: missing, duplicated or reordered bus clock")
            digest.update(name.encode() + b"\0" + ours)
            count += 1
        actual = digest.hexdigest()
        print(f"{count} SPC bus-cycle cases: {actual}")
        if actual != (RESUMABLE_EXPECTED if args.resumable else EXPECTED):
            raise AssertionError("SPC bus-cycle corpus changed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"SPC bus-cycle test failed: {error}", file=sys.stderr)
        sys.exit(1)
