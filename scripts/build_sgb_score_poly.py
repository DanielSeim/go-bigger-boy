#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned audio with independently re-keyed bounded native tracks."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_multi import ROOT, source as multi_source


def build():
    source = multi_source()
    hooks = {
        'multi_duration:\n': 'multi_duration:\n    cmp a, #$02\n    bcs poly_duration_ok\n    jmp pair_reject\npoly_duration_ok:\n',
        'multi_opcode:\n    mov $26, a\n': 'multi_opcode:\n    call duet_validate\n    mov $26, a\n',
        '    mov $64, #$01\n': '    call poly_setup\n    mov $64, #$01\n',
        'multi_pattern:\n': 'multi_pattern:\n    call poly_all\n',
        '    call multi_load2\n    call multi_load3\n    ret\n':
            '    call multi_load2\n    call multi_load3\n    call poly_on\n    ret\n',
        '    beq multi_end3\n    mov a, $32\n': '    beq multi_end3\n    call poly_prepare\n    mov a, $32\n',
        'multi_tick_done:\n': 'multi_tick_done:\n    call poly_on\n',
        'multi_complete:\n': 'multi_complete:\n    call poly_complete\n',
        '    inc $28\n    ret\n': '    inc $28\n    call duet_voice\n    ret\n',
    }
    for old, new in hooks.items():
        if source.count(old) != 1:
            raise ValueError('experimental polyphonic hook changed')
        source = source.replace(old, new)
    source += '\n' + (ROOT/'firmware/sgb/score_poly.asm').read_text()
    source += '\n' + (ROOT/'firmware/sgb/score_duet.asm').read_text()
    image = bytes(assemble(source, 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental polyphonic image exceeds code/source bound')
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
    print(f'Experimental score poly: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
