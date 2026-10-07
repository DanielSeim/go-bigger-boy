#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an isolated native two-pattern N-SPC bank parser and muted scheduler."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble

ROOT = Path(__file__).resolve().parents[1]


def build():
    clock = (ROOT/'firmware/sgb/score_clock.asm').read_text()
    hooks = {
        'clock_start:\n': 'clock_start:\n    call pair_start\n',
        '    bne clock_next\n    inc $11\n':
            '    bne clock_dispatch\n    inc $11\nclock_dispatch:\n    call pair_tick\n',
    }
    for old, new in hooks.items():
        if clock.count(old) != 1:
            raise ValueError('experimental clock hook changed')
        clock = clock.replace(old, new)
    pair = (ROOT/'firmware/sgb/score_pair.asm').read_text()
    if pair.count('pair_reject:\n') != 1 or pair.count('mov a, $2b00+x') != 6:
        raise ValueError('experimental pair hook changed')
    # Retain the countdown/transition routines; replace diagnostic input validation.
    pair = 'pair_reject:\n' + pair.split('pair_reject:\n')[1]
    if pair.count('mov a, $2b00+x') != 4:
        raise ValueError('experimental pair event reader changed')
    pair = pair.replace('mov a, $2b00+x', 'mov a, $3100+x')
    track = (ROOT/'firmware/sgb/score_track.asm').read_text()
    if track.count('track_emit:\n') != 1:
        raise ValueError('experimental event writer hook changed')
    source = clock + '\n' + (ROOT/'firmware/sgb/score_phrase.asm').read_text()
    source += '\n' + pair + '\ntrack_emit:\n' + track.split('track_emit:\n')[1]
    image = bytes(assemble(source, 'spc', 0x0800))
    if len(image) > 1024:
        raise ValueError('experimental phrase program exceeds code bound')
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
    print(f'Experimental score phrase: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
