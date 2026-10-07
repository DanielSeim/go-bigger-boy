#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated owned two-voice audio on the bounded native phrase parser."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_phrase import ROOT, source as phrase_source


def build():
    source = phrase_source()
    hooks = {
        'phrase_valid:\n': 'phrase_valid:\n    call duet_setup\n',
        'phrase_duration:\n': 'phrase_duration:\n    cmp a, #$02\n    bcs duet_duration_valid\n    jmp pair_reject\nduet_duration_valid:\n',
        'phrase_note:\n': 'phrase_note:\n    call duet_validate\n',
        'pair_pattern:\n': 'pair_pattern:\n    call duet_stop\n    call duet_wait\n    mov $51, #$00\n',
        '    inc $30\n    call track_emit\n    ret\npair_tick:\n':
            '    inc $30\n    call track_emit\n    call duet_on\n    ret\npair_tick:\n',
        'pair_finished:\n': 'pair_finished:\n    call duet_stop\n',
        '    inc $28\n    ret\n': '    inc $28\n    call duet_voice\n    ret\n',
    }
    for old, new in hooks.items():
        if source.count(old) != 1:
            raise ValueError('experimental phrase audio hook changed')
        source = source.replace(old, new)
    source += '\n' + (ROOT/'firmware/sgb/score_duet.asm').read_text()
    image = bytes(assemble(source, 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental duet exceeds code/source bound')
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
    print(f'Experimental score duet: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
