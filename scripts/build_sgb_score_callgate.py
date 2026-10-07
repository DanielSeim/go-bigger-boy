#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build independent gated audio with bounded native finite calls."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_polygate import ROOT, source as gate_source
from build_sgb_score_calls import expand_cache


def source():
    text = gate_source()
    parser = (ROOT/'firmware/sgb/score_calls.asm').read_text()
    # Preserve live gate counters and lookup scratch; parser uses $7C..80.
    for old, new in zip(('72', '73', '74', '75', '76'), ('7c', '7d', '7e', '7f', '80')):
        parser = parser.replace('$'+old, '$'+new)
    hooks = {
        '    mov $68, a\n':
            '    cmp a, #$02\n    bcs callgate_duration_ok\n    jmp pair_reject\ncallgate_duration_ok:\n    mov $68, a\n',
        '    cmp a, #$7f\n    beq calls_articulation\n':
            '    cmp a, #$3f\n    beq calls_articulation\n    cmp a, #$7f\n    beq calls_articulation\n',
        '    mov $69, #$01\n': '    mov $69, a\n',
        'calls_timed:\n':
            'calls_timed:\n    call duet_validate\n    call gates_validate\n    call gates_cache\n',
    }
    for old, new in hooks.items():
        if parser.count(old) != 1:
            raise ValueError('experimental call gate parser hook changed')
        parser = parser.replace(old, new)
    first, last = 'phrase_track:\n', 'multi_start:\n'
    if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
        raise ValueError('experimental call gate parser boundary changed')
    text = text[:text.index(first)] + parser + '\n' + text[text.index(last):]
    return expand_cache(text)


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental call gate image exceeds code/source bound')
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
    print(f'Experimental gated calls: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
