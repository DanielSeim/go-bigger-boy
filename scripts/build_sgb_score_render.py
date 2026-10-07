#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an isolated native renderer with an independently authored BRR source."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_track import ROOT, source


def build():
    assembly = source()
    hooks = {
        '    mov $14, #$01\n    call track_next\n':
            '    call render_setup\n    mov $14, #$01\n    call track_next\n',
        'track_timed:\n': 'track_timed:\n    call render_validate\n',
        'track_finished:\n': 'track_finished:\n    call render_stop\n',
        '    inc $28\n    ret\n': '    inc $28\n    call render_event\n    ret\n',
    }
    for old, new in hooks.items():
        if assembly.count(old) != 1:
            raise ValueError('experimental track renderer hook changed')
        assembly = assembly.replace(old, new)
    assembly += '\n' + (ROOT/'firmware/sgb/score_render.asm').read_text()
    image = bytes(assemble(assembly, 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental renderer image exceeds code/source bound')
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
    print(f'Experimental score renderer: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
