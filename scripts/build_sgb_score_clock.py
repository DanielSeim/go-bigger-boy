#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the isolated experimental SPC score clock; never modify bundled firmware."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble

ROOT = Path(__file__).resolve().parents[1]


def build():
    return bytes(assemble((ROOT/'firmware/sgb/score_clock.asm').read_text(), 'spc', 0x0800))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Experimental score clock: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':
    main()
