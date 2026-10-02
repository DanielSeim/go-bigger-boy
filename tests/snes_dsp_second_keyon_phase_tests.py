#!/usr/bin/env python3
"""ROM-free voice-2 retrigger comparison at identical DSP cycle boundaries."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_snes_dsp_pcm import build_reference


EXPECTED = "1a4d66d373ad299283f2f3684da0c5d94af490ecf442841c6ade48483ecade1c"


def stimulus(phase):
    # Original looping BRR data, not title/firmware bytes. Voice 2 uses the
    # 1,437-unit pitch seen at the title's disputed checkpoint.
    lines = ["ram 256 0", "ram 257 2", "ram 258 0", "ram 259 2",
             "ram 512 131"]
    lines += [f"ram {513+i} {value}" for i, value in enumerate(
        (0x17, 0x3a, 0x5c, 0x7e, 0x91, 0xb3, 0xd5, 0xf7))]
    lines += ["reg 93 1", "reg 108 32", "reg 12 127", "reg 28 127",
              "reg 32 100", "reg 33 90", "reg 34 157", "reg 35 5",
              "reg 36 0", "reg 37 143", "reg 38 224", "reg 39 0", "reg 76 4",
              f"clock {3072+phase}", "keyclock", "reg 76 4"]
    # Count output boundaries (phase 27), not rounded milliseconds. The
    # steady-state checkpoint is 640 outputs after the second register write.
    first_output = ((27-phase) % 32) + 1
    # Observe acceptance/startup directly for the first eight outputs, not
    # just the final steady-state position. Every state read is at phase 28.
    for index in range(8):
        lines += [f"clock {first_output if index == 0 else 32}",
                  "state 2 0", "state 2 1", "state 2 2"]
    lines += [f"clock {632 * 32}", "state 2 0", "state 2 1", "state 2 2",
              "clock 32", "state 2 0", "state 2 1", "state 2 2"]
    return ("\n".join(lines) + "\n").encode("ascii")


def run(binary, data, phase):
    result = subprocess.run([str(binary)], input=data, capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError(result.stderr.decode(errors="replace"))
    states = [int(line.split()[3]) for line in result.stderr.splitlines()
              if line.startswith(b"state ")]
    clocks = [int(line.split()[1]) for line in result.stderr.splitlines()
              if line.startswith(b"keyclock ")]
    if clocks != [phase]:
        raise AssertionError("key-poll parity/phase is not aligned")
    if len(states) != 30 or len(result.stdout) % 4:
        raise AssertionError("incomplete PCM or voice state")
    if not any(states[index] == 0 for index in range(0, 24, 3)):
        raise AssertionError("checkpoint did not observe key-on startup silence")
    if states[-6] != 2047 or states[-3] != 2047:
        raise AssertionError("checkpoint did not exercise an active voice")
    if (states[-1] - states[-4]) % 16384 != 1437:
        raise AssertionError("one additional sample must advance exactly one pitch step")
    return result.stdout, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()
    title_phase_positions = {}
    with tempfile.TemporaryDirectory(prefix="gbb-second-kon-phase-") as temporary:
        reference = Path(temporary) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir, reference)
        # Both alternate-sample KON poll parities and every write phase.
        for phase in range(64):
            data = stimulus(phase)
            ours = run(args.gbb.resolve(), data, phase)
            if args.reference_dir:
                theirs = run(reference, data, phase)
                if ours != theirs:
                    raise AssertionError(f"phase {phase}: PCM/internal state differs; "
                                         f"GBB {ours[1]!r}, reference {theirs[1]!r}")
            digest.update(ours[0])
            digest.update(ours[1])
            if phase in (41, 62):
                values = [int(line.split()[3]) for line in ours[1].splitlines()
                          if line.startswith(b"state ")]
                title_phase_positions[phase] = values[-4]
        if title_phase_positions != {41: 8501, 62: 9938}:
            raise AssertionError("the title's one-step phase difference was not reproduced")
        print("Title write phases 41/62 reproduce positions 8501/9938 with identical synthetic data")
        actual = digest.hexdigest()
        print(f"64 second-KON phases: PCM/state SHA-256 {actual}")
        if actual != EXPECTED:
            raise AssertionError("second-KON PCM/state corpus changed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"second-KON phase test failed: {error}", file=sys.stderr)
        sys.exit(1)
