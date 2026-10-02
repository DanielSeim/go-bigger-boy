#!/usr/bin/env python3
"""Original, ROM-free timer/accumulator carry branch; optional independent SPC."""
import argparse
import hashlib
from pathlib import Path
import tempfile

from snes_spc_half_cycle_tests import run
from snes_spc_write_cycle_tests import build_reference

EXPECTED = "2d4c78b8848b50e232b9ce326552d34f1cd4d410d36cd3f2ea70ec1c6f229e89"
BOUNDARY_EXPECTED = "d321d6de7773e14bfbbd66c42554a0eac466b40109ce75f6194c02be9d5a4976"


def boundary_fixture(padding):
    # Two polls straddle a free-running timer boundary. Multiplying each
    # returned count by 44 preserves total phase when a tick moves to the
    # following poll. These bytes are original stimuli, not firmware code.
    update = bytes((0xfd, 0xe8, 44, 0xcf, 0x60, 0x84, 0x43, 0xc4, 0x43))
    program = (bytes((0x8f, 1, 0xfa, 0x8f, 1, 0xf1)) + bytes(padding) +
               bytes((0xe4, 0, 0xe4, 0xfd)) + update + bytes(61) +
               bytes((0xe4, 0xfd)) + update)
    lines = ["ram 65472 95", "ram 65473 0", "ram 65474 4", "ram 67 156"]
    lines += [f"ram {0x400+i} {value}" for i, value in enumerate(program)]
    lines.append(f"run {padding+79}")
    return ("\n".join(lines) + "\n").encode()


def fixture(phase):
    # Jump to original RAM code: the program deliberately exceeds the IPL size.
    program = (bytes((0x8f, 1, 0xfa, 0x8f, 1, 0xf1)) + bytes(59) +
               bytes((0xe4, 0xfd, 0x60, 0xe4, 0x43, 0x88, 44, 0xc4, 0x43,
                      0x90, 10, 0xe4, 0xd8, 0xbc, 0xc4, 0xd8,
                      0x8f, 0x0c, 0xf2, 0xc4, 0xf3)) + bytes(8))
    lines = ["ram 65472 95", "ram 65473 0", "ram 65474 4",
             f"ram 67 {phase}", "ram 216 3"]
    lines += [f"ram {0x400+i} {value}" for i, value in enumerate(program)]
    lines.append("run 73")
    return ("\n".join(lines) + "\n").encode()


def check(trace, phase, wraps):
    rows = [line.split() for line in trace.splitlines()]
    polls = [row for row in rows if row[0] == b"R" and int(row[2]) == 0xfd]
    if len(polls) != 1 or int(polls[0][3]) != 1:
        raise AssertionError("identical timer tick was not observed")
    writes = [row for row in rows if row[0] == b"W"]
    phase_writes = [int(row[4]) for row in writes if int(row[3]) == 0x43]
    volume = [(int(row[1]), int(row[4])) for row in writes if int(row[3]) == 0xf3]
    if phase_writes != [(phase + 44) & 255] or bool(volume) != wraps:
        raise AssertionError("carry branch did not follow the inherited phase")
    if wraps and (len(volume) != 1 or volume[0][1] != 4):
        raise AssertionError("carry branch wrote the wrong volume")
    return tuple(polls[0][1:]), volume


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    common_poll = None
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="gbb-timer-phase-") as directory:
        reference = Path(directory) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir.resolve(), reference)
        for phase, wraps in ((252, True), (192, False), (236, True)):
            data = fixture(phase)
            ours = run(args.gbb.resolve(), data)
            if args.reference_dir and ours != run(reference, data):
                raise AssertionError(f"phase {phase}: independent half-cycle trace differs")
            poll, volume = check(ours, phase, wraps)
            if common_poll is not None and poll != common_poll:
                raise AssertionError("timer observation changed with accumulator state")
            common_poll = poll
            digest.update(bytes((phase,)) + ours)
            print(f"phase {phase} -> {(phase+44)&255}: timer={poll}, volume={volume}")
        boundary_digest = hashlib.sha256()
        for padding, ticks, phases in ((118, [1, 2], [200, 32]), (119, [2, 1], [244, 32])):
            data = boundary_fixture(padding)
            ours = run(args.gbb.resolve(), data)
            if args.reference_dir and ours != run(reference, data):
                raise AssertionError("independent timer-boundary trace differs")
            rows = [line.split() for line in ours.splitlines()]
            observed_ticks = [int(r[3]) for r in rows if r[0] == b"R" and int(r[2]) == 0xfd]
            observed_phases = [int(r[4]) for r in rows if r[0] == b"W" and int(r[3]) == 0x43]
            if observed_ticks != ticks or observed_phases != phases:
                raise AssertionError(f"timer boundary: {observed_ticks}, {observed_phases}")
            boundary_digest.update(bytes((padding,)) + ours)
        print(f"2 timer-boundary cases: {boundary_digest.hexdigest()}")
        if boundary_digest.hexdigest() != BOUNDARY_EXPECTED:
            raise AssertionError("independently checked timer-boundary trace changed")
    if digest.hexdigest() != EXPECTED:
        raise AssertionError("independently checked timer/phase trace changed")
    print("5 timer/phase cases passed; no firmware bytes or phase reset used.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
