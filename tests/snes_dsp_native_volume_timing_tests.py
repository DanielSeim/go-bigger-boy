#!/usr/bin/env python3
"""Original synthetic tone: accepted master-volume timing is audible, not a rate fit."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_snes_dsp_pcm import build_reference

EXPECTED = "01b392b103856a42f545a6c3fdc2f4104f9650a16ab7a9d8473e3ada5672543e"


def fixture(left_clock, right_clock):
    # Original repeating BRR tone, not bytes extracted from a title or firmware.
    lines = ["ram 0x2800 0", "ram 0x2801 0x80", "ram 0x2802 0", "ram 0x2803 0x80",
             "ram 0x8000 0x83"]
    lines += [f"ram {0x8000+i} 0x77" for i in range(1, 9)]
    lines += ["reg 0x5d 0x28", "reg 0x6c 0x20", "reg 0xc 3", "reg 0x1c 3",
              "reg 0x20 0x7f", "reg 0x21 0x7f", "reg 0x22 0x9d", "reg 0x23 5",
              "reg 0x24 0", "reg 0x25 0", "reg 0x27 0x7f", "clock 37", "reg 0x4c 4",
              f"clock {left_clock}", "reg 0xc 4", f"clock {right_clock-left_clock}",
              "reg 0x1c 4", f"clock {4096-right_clock}"]
    return ("\n".join(lines) + "\n").encode()


def run(binary, data):
    return subprocess.run([str(binary)], input=data, capture_output=True,
                          check=True, timeout=30).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()
    outputs = []
    with tempfile.TemporaryDirectory(prefix="gbb-native-volume-") as directory:
        reference = Path(directory) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir.resolve(), reference)
        for left, right in ((1521, 1546), (3568, 3593)):
            data = fixture(left, right)
            pcm = run(args.gbb.resolve(), data)
            if args.reference_dir and pcm != run(reference, data):
                raise AssertionError("same accepted writes did not produce independent byte-exact PCM")
            if len(pcm) != 129 * 4 or not any(pcm):
                raise AssertionError("expected 129 non-silent stereo output frames")
            digest.update(pcm)
            outputs.append(pcm)
        first = next((i for i in range(129) if outputs[0][4*i:4*i+4] != outputs[1][4*i:4*i+4]), None)
        if first != 48:
            raise AssertionError(f"first differing output frame moved: {first}")
        actual = digest.hexdigest()
        print(f"2 native master-volume timing cases: {actual}; first differing frame {first}")
        if actual != EXPECTED:
            raise AssertionError("native master-volume timing corpus changed")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"native volume test failed: {error}", file=sys.stderr)
        sys.exit(1)
