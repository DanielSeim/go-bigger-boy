#!/usr/bin/env python3
"""ROM-free replacement IPL upload -> synthetic DSP voice PCM contract."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="gbb-replacement-ipl-pcm-") as temporary:
        image = Path(temporary) / "gbb-ipl.bin"
        subprocess.run([sys.executable, str(root / "scripts/build_spc700_ipl.py"),
                        "--check", "--output", str(image)], check=True)
        subprocess.run([sys.executable, str(root / "tests/snes_spc700_real_ipl_pcm_tests.py"),
                        sys.argv[1], str(image), *sys.argv[2:]], check=True)


if __name__ == "__main__":
    main()
