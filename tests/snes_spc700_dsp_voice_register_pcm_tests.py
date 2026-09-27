#!/usr/bin/env python3
"""Compare clock-timed two-voice register writes with independent S-DSP PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_subsample_pcm_tests import run


EXPECTED_PCM_SHA256 = "4782425c23ae005b19509cc81694bcafffde8c9f71faee4fe646bd5224ec4b40"
EXPECTED_ENDX_SHA256 = "f6409dc71bf4b99a5e14649c7570d9e91b145f7b916bbdc7bb67fef8ddeeed1e"
EXPECTED_CPU_PCM_SHA256 = "d5d8796ff1d19e06543e0e083eec53370f4e15c669a7998639eea4dfa0c4afca"


def static_fixture(timeline: Path) -> bytes:
    baseline = subprocess.check_output(
        [str(timeline), "--multi-key-clock-fixture"], timeout=30)
    marker = b"clock 2048\n"
    if baseline.count(marker) != 1:
        raise AssertionError("multi-voice static fixture changed")
    alternate_samples = (
        b"ram 0x2808 0x00\nram 0x2809 0x82\n"
        b"ram 0x280a 0x00\nram 0x280b 0x82\n"
        b"ram 0x280c 0x00\nram 0x280d 0x83\n"
        b"ram 0x280e 0x00\nram 0x280f 0x83\n"
        b"ram 0x8200 0x83\nram 0x8300 0x83\n"
    )
    for address in range(0x8201, 0x8209):
        alternate_samples += f"ram {address} 0x77\n".encode()
    for address in range(0x8301, 0x8309):
        alternate_samples += f"ram {address} 0x11\n".encode()
    return baseline.split(marker, 1)[0] + alternate_samples + marker


def all_voice_fixture(static: bytes) -> bytes:
    marker = b"clock 2048\n"
    if static.count(marker) != 1:
        raise AssertionError("two-voice setup changed")
    extra: list[str] = []
    for voice in range(2, 8):
        sample = 0x8000 + voice * 0x100
        directory = 0x2800 + voice * 4
        for offset in (0, 2):
            extra.append(f"ram {directory+offset} {sample & 0xff}")
            extra.append(f"ram {directory+offset+1} {sample >> 8}")
        extra.append(f"ram {sample} 0x83")
        for offset in range(1, 9):
            extra.append(f"ram {sample+offset} {(offset * 37 + voice * 29) & 0xff:#x}")
        base = voice * 0x10
        for offset, value in ((0, 0x18), (1, 0x18), (2, 0),
                              (3, 0x10 + voice), (4, voice), (7, 0x7f)):
            extra.append(f"reg {base+offset} {value}")
    for source in range(8, 16):
        sample = 0x8000 + source * 0x100
        directory = 0x2800 + source * 4
        for offset in (0, 2):
            extra.append(f"ram {directory+offset} {sample & 0xff}")
            extra.append(f"ram {directory+offset+1} {sample >> 8}")
        extra.append(f"ram {sample} 0x83")
        for offset in range(1, 9):
            extra.append(f"ram {sample+offset} {(offset * 53 + source * 17) & 0xff:#x}")
    extra.append("reg 0x4c 0xff")
    return static.replace(marker, ("\n".join(extra) + "\n").encode() + marker)


def event(stimulus: bytes, address: int, value: int, clock: int,
          duration: int = 512) -> bytes:
    if clock < 0 or clock >= duration:
        raise AssertionError("invalid write clock")
    prefix = f"clock {clock}\n".encode() if clock else b""
    return stimulus + prefix + f"reg {address} {value}\nclock {duration-clock}\n".encode()


def cases(static: bytes) -> dict[str, bytes]:
    result = {"baseline": static + b"clock 512\n"}
    specs = {
        "v0_left": (0x00, 0x20, 31),
        "v0_right": (0x01, 0x20, 32),
        "v1_left": (0x10, 0x20, 2),
        "v1_right": (0x11, 0x20, 3),
        "v0_pitch_low": (0x02, 0x80, 21),
        "v0_pitch_high": (0x03, 0x18, 22),
        "v1_pitch_low": (0x12, 0xC0, 0),
        "v1_pitch_high": (0x13, 0x1B, 1),
        "v0_gain": (0x07, 0x40, 30),
        "v1_gain": (0x17, 0x40, 1),
        "v0_adsr0": (0x05, 0x8F, 21),
        "v1_adsr0": (0x15, 0x8F, 0),
    }
    for name, (address, value, phase) in specs.items():
        result[f"{name}_before"] = event(static, address, value, phase)
        result[f"{name}_after"] = event(static, address, value, phase + 1)
    for voice, address, source, phase in ((0, 0x04, 2, 49),
                                          (1, 0x14, 3, 52)):
        setup = static.replace(b"clock 2048\n", b"clock 2080\n")
        for suffix, clock in (("before", phase), ("after", phase + 1)):
            result[f"v{voice}_source_{suffix}"] = (
                setup + b"clock 25\n" +
                f"reg 76 {1 << voice}\nclock {clock-25}\n".encode() +
                f"reg {address} {source}\nclock {512-clock}\n".encode())
    adsr_static = static.replace(
        b"clock 2048\n",
        b"reg 0x05 0xff\nreg 0x06 0xe0\n"
        b"reg 0x15 0xff\nreg 0x16 0xe0\nclock 2048\n")
    for voice, address, phase in ((0, 0x06, 30), (1, 0x16, 1)):
        result[f"v{voice}_adsr1_before"] = event(adsr_static, address, 0xff,
                                                 phase, 2048)
        result[f"v{voice}_adsr1_after"] = event(adsr_static, address, 0xff,
                                                phase + 1, 2048)
    return result


def all_voice_cases(static: bytes) -> dict[str, bytes]:
    result = {"all_baseline": static + b"clock 512\n"}
    for voice in range(2, 8):
        base = voice * 0x10
        phase = 4 + 3 * (voice - 2)  # V3c
        adsr_sample_offset = {2: 0, 3: 64, 4: 32, 5: 0, 6: 64, 7: 0}[voice]
        specs = {
            "left": (0, 0x08, phase + 1),
            "right": (1, 0x08, phase + 2),
            "pitch_low": (2, 0x80, phase - 1),
            "pitch_high": (3, 0x18, phase),
            "gain": (7, 0x40, phase),
            # Pick a sample where the global envelope-rate counter makes
            # the otherwise one-sample ADSR0 read-window difference audible.
            "adsr0": (5, 0x8f, phase - 1 + adsr_sample_offset),
        }
        for kind, (offset, value, read_phase) in specs.items():
            for suffix, clock in (("before", read_phase),
                                  ("after", read_phase + 1)):
                result[f"all_v{voice}_{kind}_{suffix}"] = event(
                    static, base + offset, value, clock)
        source_phase = 63 if voice == 2 else 66 + 3 * (voice - 3)
        setup = static.replace(b"clock 2048\n", b"clock 2080\n")
        for suffix, clock in (("before", source_phase),
                              ("after", source_phase + 1)):
            result[f"all_v{voice}_source_{suffix}"] = (
                setup + b"clock 25\n" +
                f"reg 76 {1 << voice}\nclock {clock-25}\n".encode() +
                f"reg {base+4} {8+voice}\nclock {512-clock}\n".encode())
        adsr_setup = static.replace(
            b"clock 2048\n",
            f"reg {base+5} 0xff\nreg {base+6} 0xe0\nclock 2048\n".encode())
        for suffix, clock in (("before", phase), ("after", phase + 1)):
            result[f"all_v{voice}_adsr1_{suffix}"] = event(
                adsr_setup, base + 6, 0xff, clock, 2048)
    return result


def with_all_voice_endx_probes(stimulus: bytes) -> bytes:
    checkpoints = sorted({32} | {phase + delta for phase in range(2, 24, 3)
                                    for delta in (0, 1)})
    clock = 0
    result: list[str] = []
    for line in stimulus.decode("ascii").splitlines():
        if not line.startswith("clock "):
            result.append(line)
            continue
        remaining = int(line.split()[1])
        while remaining:
            position = clock % 32
            target = next(value for value in checkpoints if value > position)
            advance = min(remaining, target - position)
            result.append(f"clock {advance}")
            clock += advance
            remaining -= advance
            if (clock % 32 or 32) in checkpoints:
                result.append("endx")
    return ("\n".join(result) + "\n").encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    static = static_fixture(args.timeline)
    with tempfile.TemporaryDirectory(prefix="gbb-snes-dsp-voice-reg-") as temp:
        reference = None
        if args.reference_dir:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            from compare_snes_dsp_pcm import build_reference
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
        all_static = all_voice_fixture(static)
        fixtures = cases(static) | all_voice_cases(all_static)
        pcm_hash = hashlib.sha256()
        observed: dict[str, bytes] = {}
        for name, stimulus in fixtures.items():
            pcm = run(args.gbb, stimulus)
            expected_samples = 128 if "_adsr1_" in name else (
                81 if "_source_" in name else 80)
            if len(pcm) != expected_samples * 4:
                raise AssertionError(f"{name}: wrong stereo sample count")
            if reference:
                expected = run(reference, stimulus)
                if pcm != expected:
                    first = next(i for i in range(expected_samples) if
                                 pcm[4*i:4*i+4] != expected[4*i:4*i+4])
                    raise AssertionError(f"{name}: PCM mismatch at sample {first}: "
                                         f"{pcm[4*first:4*first+4].hex()} != "
                                         f"{expected[4*first:4*first+4].hex()}")
            pcm_hash.update(name.encode() + b"\0")
            pcm_hash.update(pcm)
            observed[name] = pcm
        for name, pcm in observed.items():
            if name.endswith("_before") and pcm == observed[name[:-7] + "_after"]:
                raise AssertionError(f"{name}: crossing its read phase had no PCM effect")
        if pcm_hash.hexdigest() != EXPECTED_PCM_SHA256:
            raise AssertionError(f"PCM corpus hash changed: {pcm_hash.hexdigest()}")
        cpu_stimulus = subprocess.check_output(
            [str(args.timeline), "--voice-register-clock-fixture"], timeout=30)
        for fragment in (b"clock 26\nreg 0 32", b"clock 33\nreg 19 27",
                         b"clock 33\nreg 7 64"):
            if fragment not in cpu_stimulus:
                raise AssertionError("SPC700 voice-register event clocks changed")
        cpu_pcm = run(args.gbb, cpu_stimulus)
        if len(cpu_pcm) != 83 * 4 or \
           hashlib.sha256(cpu_pcm).hexdigest() != EXPECTED_CPU_PCM_SHA256:
            raise AssertionError("SPC700-produced voice-register PCM changed")
        if reference and cpu_pcm != run(reference, cpu_stimulus):
            raise AssertionError("SPC700-produced voice-register PCM differs from DSP")
        probes = ["all_baseline"] + [
            f"all_v{voice}_source_{suffix}"
            for voice in range(2, 8) for suffix in ("before", "after")]
        endx_hash = hashlib.sha256()
        for name in probes:
            stimulus = with_all_voice_endx_probes(fixtures[name])
            ours = subprocess.run([str(args.gbb)], input=stimulus,
                                  capture_output=True, check=True, timeout=30)
            expected_count = 81 if "_source_" in name else 80
            lines = ours.stderr.splitlines()
            if len(lines) != expected_count * 17 or any(
                    not line.startswith(b"endx ") for line in lines):
                raise AssertionError(f"{name}: wrong ENDX probe trace")
            if reference:
                theirs = subprocess.run([str(reference)], input=stimulus,
                                        capture_output=True, check=True, timeout=30)
                if ours.stdout != theirs.stdout or ours.stderr != theirs.stderr:
                    first = next((i for i, (a, b) in enumerate(zip(
                        lines, theirs.stderr.splitlines())) if a != b), None)
                    raise AssertionError(f"{name}: eight-voice PCM/ENDX mismatch "
                                         f"at probe {first}")
            if name == "all_baseline":
                values = [int(line.split()[1]) for line in lines]
                transitions = [(i, value) for i, value in enumerate(values)
                               if i == 0 or value != values[i-1]]
                if transitions != [(0, 0), (200, 64), (202, 192),
                                   (205, 193), (207, 195), (209, 199),
                                   (211, 207), (213, 223), (215, 255)]:
                    raise AssertionError(f"eight-voice ENDX phases changed: "
                                         f"{transitions}")
            endx_hash.update(name.encode() + b"\0")
            endx_hash.update(ours.stderr)
        if endx_hash.hexdigest() != EXPECTED_ENDX_SHA256:
            raise AssertionError(f"ENDX corpus hash changed: {endx_hash.hexdigest()}")
        for address in (0x08, 0x09, 0x1D, 0x7C):
            invalid = static + f"clock 1\nreg {address} 1\nclock 511\n".encode()
            rejected = subprocess.run([str(args.gbb)], input=invalid,
                                      capture_output=True, timeout=30)
            if rejected.returncode != 3 or b"unsupported DSP mode" not in rejected.stderr:
                raise AssertionError(f"unmodeled timed register {address:#x} was accepted")
        print(f"{len(fixtures) + 1} PCM traces and {len(probes)} ENDX traces matched "
              "pinned results" + (" and the independent DSP" if reference else ""))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"timed voice-register PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
