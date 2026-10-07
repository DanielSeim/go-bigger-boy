#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build independent polyphonic gates using ten measured articulation profiles."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_poly import ROOT, source as poly_source


def source():
    source = poly_source()
    hooks = {
        '    cmp a, #$7f\n    bne multi_parse_bad\n    mov $69, #$01\n':
            '    cmp a, #$3f\n    beq gates_articulation\n    cmp a, #$7f\n    bne multi_parse_bad\ngates_articulation:\n    mov $69, a\n',
        '    inc $63\n    call track_emit\n':
            '    inc $63\n    call gates_art_event\n    call track_emit\n',
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
        '.org $1000\n': '.org $0f80\n; tempo, articulation, duration, timer pulses.\n'
            '.byte $60, $7f, $08, $0f, $60, $3f, $08, $0a\n'
            '.byte $60, $7f, $10, $24, $60, $3f, $10, $17\n'
            '.byte $60, $7f, $18, $3a, $60, $3f, $18, $25\n'
            '.byte $80, $7f, $10, $1b, $80, $3f, $10, $11\n'
            '.byte $c0, $7f, $10, $12, $c0, $3f, $10, $0b\n.org $1000\n',
    }
    for old, new in hooks.items():
        if source.count(old) != 1:
            raise ValueError('experimental polyphonic gate hook changed')
        source = source.replace(old, new)
    return source


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
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
