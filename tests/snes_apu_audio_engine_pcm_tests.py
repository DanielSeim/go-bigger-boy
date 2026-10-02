#!/usr/bin/env python3
"""Integrated SPC-generated PCM against the existing independent DSP protocol."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
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
