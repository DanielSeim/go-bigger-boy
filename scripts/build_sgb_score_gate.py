#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated native calibrated articulation profiles."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_render import ROOT, source as render_source


def source():
    assembly = render_source()
    hooks = {
        '    cmp a, #$c0\n    beq clock_start\n':
            '    cmp a, #$80\n    beq clock_start\n    cmp a, #$c0\n    beq clock_start\n',
        '    call render_validate\n': '    call gate_validate\n',
        'clock_pulse:\n': 'clock_pulse:\n    call gate_pulse\n',
        'render_setup:\n': 'render_setup:\n    mov $2d, #$00\n',
        'render_stop:\n': 'render_stop:\n    mov $2d, #$00\n',
        '    mov $f3, #$04\nrender_done:\n':
            '    mov $f3, #$04\n    call gate_start\nrender_done:\n',
        '.org $1000\n': (ROOT/'firmware/sgb/score_gate.asm').read_text() + '\n.org $1000\n',
    }
    for old, new in hooks.items():
        if assembly.count(old) != 1:
            raise ValueError('experimental articulation hook changed')
        assembly = assembly.replace(old, new)
    return assembly


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental gate image exceeds code/source bound')
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
    print(f'Experimental score gates: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
