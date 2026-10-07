#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build independent polyphonic gates using five measured articulation-127 profiles."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_poly import ROOT, source as poly_source


def build():
    source = poly_source()
    hooks = {
        '    cmp a, #$c0\n    beq clock_start\n':
            '    cmp a, #$80\n    beq clock_start\n    cmp a, #$c0\n    beq clock_start\n',
        'clock_pulse:\n': 'clock_pulse:\n    call gates_pulse\n',
        'multi_timed:\n': 'multi_timed:\n    call gates_validate\n    call gates_cache\n',
        'poly_setup:\n': (ROOT/'firmware/sgb/score_polygate.asm').read_text() + '\npoly_setup:\n    call gates_init\n',
        '    ; Remove prior KON data; assert KOF only for changed voices.\n':
            '    ; Remove prior KON data; assert KOF only for changed voices.\n    call gates_cancel\n',
        '    inc $28\n    call duet_voice\n': '    inc $28\n    call gates_event\n    call duet_voice\n',
        '    mov $f3, a\n    ret\npoly_complete:\n': '    mov $f3, a\n    call gates_on\n    ret\npoly_complete:\n',
        'poly_complete:\n': 'poly_complete:\n    call gates_init\n',
        '.org $1000\n': '.org $0f80\n; tempo, duration, timer pulses; articulation 127 only.\n'
            '.byte $60, $08, $0f, $60, $10, $24, $60, $18, $3a\n'
            '.byte $80, $10, $1b, $c0, $10, $12\n.org $1000\n',
    }
    for old, new in hooks.items():
        if source.count(old) != 1:
            raise ValueError('experimental polyphonic gate hook changed')
        source = source.replace(old, new)
    image = bytes(assemble(source, 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental polyphonic gate image exceeds code/source bound')
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
    print(f'Experimental polyphonic gates: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
