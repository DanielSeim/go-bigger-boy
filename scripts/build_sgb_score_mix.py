#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build measured per-voice pan/volume for gated chromatic finite-call playback."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_chromatic import ROOT, source as chromatic_source


def source():
    text = chromatic_source()
    first, last = 'phrase_control:\n', 'pair_reject:\n'
    if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
        raise ValueError('experimental mix control boundary changed')
    text = text[:text.index(first)]+'phrase_control:\n    jmp mix_command\n'+text[text.index(last):]
    hooks = {
        'pair_start:\n': 'pair_start:\n    call mix_init\n',
        'phrase_track:\n': 'phrase_track:\n    call mix_track\n',
        '    mov $4c, a\n    mov a, $67\n':
            '    mov $4c, a\n    cmp a, #$e1\n    beq mix_control_dispatch\n'
            '    cmp a, #$ed\n    beq mix_control_dispatch\n    mov a, $67\n',
        '    call phrase_control\n    jmp calls_next\n':
            'mix_control_dispatch:\n    call phrase_control\n    jmp calls_next\n',
        '    call gates_cache\n': '    call gates_cache\n    call mix_validate\n    call mix_cache\n',
        '    call gates_art_event\n': '    call gates_art_event\n    call mix_event\n',
        '    call gates_event\n    call duet_voice\n':
            '    call gates_event\n    call mix_voice\n    call duet_voice\n',
        '    mov $f2, #$20\n    mov $f3, #$50\n': '    mov $f2, #$20\n    mov $f3, #$00\n',
        '    mov $f2, #$31\n    mov $f3, #$50\n': '    mov $f2, #$31\n    mov $f3, #$00\n',
    }
    for old, new in hooks.items():
        if text.count(old) != 1:
            raise ValueError('experimental mix source hook changed')
        text = text.replace(old, new)
    return text+'\n.org $1100\n'+(ROOT/'firmware/sgb/score_mix.asm').read_text()


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental mix driver exceeds code/source bound')
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
    print(f'Experimental per-voice mix: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
