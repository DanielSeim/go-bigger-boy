#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an isolated native single-track parser on the owned SPC clock."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble

ROOT = Path(__file__).resolve().parents[1]


def build():
    source = (ROOT/'firmware/sgb/score_clock.asm').read_text()
    # Fixed, checked assembly hooks keep the standalone clock artifact unchanged.
    hooks = {
        'clock_start:\n': 'clock_start:\n    call track_start\n',
        '    bne clock_next\n    inc $11\n':
            '    bne clock_dispatch\n    inc $11\nclock_dispatch:\n    call track_tick\n',
    }
    for old, new in hooks.items():
        if source.count(old) != 1:
            raise ValueError('experimental clock hook changed')
        source = source.replace(old, new)
    source += '\n' + (ROOT/'firmware/sgb/score_track.asm').read_text()
    image = bytes(assemble(source, 'spc', 0x0800))
    if len(image) > 1024:
        raise ValueError('experimental track program exceeds code bound')
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build()
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Experimental score track: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
