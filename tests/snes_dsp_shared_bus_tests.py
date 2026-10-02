#!/usr/bin/env python3
"""ROM-free live DSP readback and shared echo-RAM boundary comparisons."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_snes_dsp_pcm import build_reference

EXPECTED = "b391ccf5e4d96feb3a557100c9f81103a0d04032d34387bddf8949ec9595bff1"


def source(voice, adsr=False, header=0x83):
    base = voice * 16
    lines = ["ram 256 0", "ram 257 2", "ram 258 0", "ram 259 2", f"ram 512 {header}"]
    lines += [f"ram {513+i} {value}" for i, value in enumerate(
        (0x17, 0x3a, 0x5c, 0x7e, 0x91, 0xb3, 0xd5, 0xf7))]
    lines += ["reg 93 1", "reg 108 32", "reg 12 127", "reg 28 127",
              f"reg {base} 100", f"reg {base+1} 90", f"reg {base+2} 157",
              f"reg {base+3} 5", f"reg {base+4} 0", f"reg {base+5} {143 if adsr else 0}",
              f"reg {base+6} 224", f"reg {base+7} 127", f"reg 76 {1 << voice}"]
    return lines


def probes(voice):
    lines = []
    for address in (voice*16+8, voice*16+9, 124):
        lines += [f"readreg {address}", f"spc 242 {address}", "spcread 243"]
    return lines


def corpus():
    for voice in range(8):
        for adsr in (False, True):
            lines = source(voice, adsr) + ["clock 1024"]
            for _ in range(128):
                lines += ["clock 1"] + probes(voice)
            yield f"readback-v{voice}-adsr{adsr}", lines
        for offset in (8, 9, 124):
            for phase in range(32):
                # ENVX/OUTX writes alter a shared pipeline latch, even when
                # directed to a different voice's visible register.
                address = ((voice+3) % 8)*16+offset if offset != 124 else 124
                lines = source(voice) + [f"clock {1024+phase}",
                                        f"reg {address} 173", f"readreg {address}"]
                for _ in range(8):
                    lines += ["clock 1"] + probes(voice) + [f"readreg {address}"]
                yield f"write-v{voice}-reg{offset}-phase{phase}", lines
    # Echo read windows (left 22, right 23) and write windows (29/30).
    # EDL=0 keeps one stereo cell; FIR0 + feedback keeps changes observable.
    for phase in range(32):
        for address in (0x3000, 0x3001, 0x3002, 0x3003):
            lines = source(2) + ["reg 109 48", "reg 125 0", "reg 108 0",
                                 "reg 44 70", "reg 60 60", "reg 13 55", "reg 15 127",
                                 "reg 77 4", f"clock {1024+phase}", f"spc {address} 173"]
            for _ in range(40):
                lines += ["clock 1"] + [f"readram {0x3000+i}" for i in range(4)]
            yield f"echo-collision-{address}-{phase}", lines
    # Physical echo beneath I/O does not alter the DSP selector/data window.
    lines = ["reg 108 0", "reg 109 0", "reg 125 0", "spc 242 140",
             "spc 243 173", "readreg 12", "spcread 242", "spcread 243",
             "readram 243", "clock 96", "spcread 242", "spcread 243"]
    yield "high-selector-read-alias-write-ignore", lines
    for phase in range(32):
        # Echo has advanced to $00F0 after sixty samples. Its right-channel
        # write crosses physical $F2/$F3 without touching the selector/DSP.
        lines = ["reg 108 0", "reg 109 0", "reg 125 1", "reg 12 127",
                 "clock 1920", "spc 242 140", "spc 243 173", f"clock {phase+1}"]
        for _ in range(34):
            lines += ["clock 1", "spcread 242", "spcread 243", "readreg 12"]
            lines += [f"readram {address}" for address in range(0xf0, 0xf4)]
        yield f"echo-under-io-{phase}", lines


def run(binary, lines, shared):
    result = subprocess.run([str(binary)] + (["--shared-bus"] if shared else []),
                            input=("\n".join(lines)+"\n").encode(),
                            capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError(f"runner failed: {result.returncode}: {result.stderr!r}")
    if len(result.stdout) % 4:
        raise AssertionError("partial PCM sample")
    return result.stdout, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()
    count = 0
    with tempfile.TemporaryDirectory(prefix="gbb-dsp-shared-") as directory:
        reference = Path(directory) / "reference"
        if args.reference_dir:
            build_reference(args.reference_dir, reference)
        for name, lines in corpus():
            ours = run(args.gbb.resolve(), lines, True)
            if args.reference_dir:
                theirs = run(reference, lines, False)
                if ours != theirs:
                    a, b = ours[1].splitlines(), theirs[1].splitlines()
                    first = next(((i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
                    raise AssertionError(f"{name}: PCM equal={ours[0] == theirs[0]}, first probe difference={first}")
            digest.update(name.encode()+b"\0"+ours[0]+ours[1])
            count += 1
        actual = digest.hexdigest()
        print(f"{count} shared DSP bus cases: {actual}")
        if actual != EXPECTED:
            raise AssertionError("shared DSP bus corpus changed")
        for command, shared, status in ((b"clock 32\nstep 1\n", True, 3),
                                        (b"clock 32\nreadreg 128\n", True, 2),
                                        (b"clock 32\nreadreg 8\n", False, 2)):
            result = subprocess.run([str(args.gbb.resolve())] +
                                    (["--shared-bus"] if shared else []),
                                    input=command, capture_output=True, timeout=30)
            if result.returncode != status:
                raise AssertionError("invalid shared/legacy probe mode was not rejected")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"shared DSP bus test failed: {error}", file=sys.stderr)
        sys.exit(1)
