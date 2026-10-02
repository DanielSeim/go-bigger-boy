#!/usr/bin/env python3
"""Integrated SPC-generated PCM against the existing independent DSP protocol."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

def validate_benchmark(path, model):
    report = json.loads(path.read_text())
    trials = report["trials"]
    if (report["format"] != "gbb-apu-firmware-performance-v1" or
            report["model"] != model or report["apu_hz"] != 1024000 or
            report["emulated_seconds_per_trial"] != 2 or len(trials) != 3):
        raise AssertionError("invalid firmware benchmark metadata")
    for trial in trials:
        if (trial["samples"] != 64000 or not 0 < trial["nonzero_samples"] <= 64000 or
                not math.isfinite(trial["seconds"]) or trial["seconds"] <= 0 or
                not math.isfinite(trial["realtime_ratio"]) or trial["realtime_ratio"] <= 0 or
                not math.isclose(trial["realtime_ratio"] * trial["seconds"], 2, rel_tol=1e-5)):
            raise AssertionError("incomplete firmware benchmark")
    if len({(t["pcm_fnv64"], t["nonzero_samples"]) for t in trials}) != 1:
        raise AssertionError("nondeterministic live firmware replay")
    return trials

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gbb-apu-benchmark-") as directory:
        report = Path(directory) / "performance.json"
        subprocess.run([str(args.engine), "--benchmark-fixture", str(report)], check=True, timeout=60)
        validate_benchmark(report, "synthetic")
        original = report.read_bytes()
        duplicate = subprocess.run([str(args.engine), "--benchmark-fixture", str(report)],
                                   capture_output=True, timeout=60)
        if duplicate.returncode != 1 or report.read_bytes() != original:
            raise AssertionError("benchmark overwrote an existing report")
    fixture = subprocess.check_output([str(args.engine), "--fixture"], timeout=30)
    pcm = subprocess.check_output([str(args.engine), "--pcm"], timeout=30)
    expected = subprocess.run([str(args.renderer)], input=fixture, capture_output=True,
                              check=True, timeout=30).stdout
    if len(pcm) != 256 * 4 or not any(pcm) or pcm != expected:
        raise AssertionError("integrated SPC/DSP output does not equal its exact clock/write timeline")
    if hashlib.sha256(pcm).hexdigest() != "ded49d6cc25f85de36ed2768075fb889d59b383b45817bae905023638fe70371":
        raise AssertionError("integrated PCM reference baseline changed")
    if args.reference_dir:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        from compare_snes_dsp_pcm import build_reference
        with tempfile.TemporaryDirectory(prefix="gbb-apu-engine-reference-") as directory:
            reference = Path(directory) / "dsp"
            build_reference(args.reference_dir, reference)
            independent = subprocess.run([str(reference)], input=fixture, capture_output=True,
                                         check=True, timeout=30).stdout
        if independent != pcm:
            raise AssertionError("integrated PCM differs from independent DSP")
    print(f"256 integrated stereo outputs: {hashlib.sha256(pcm).hexdigest()}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
