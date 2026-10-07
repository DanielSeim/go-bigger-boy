#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated muted bounded multi-event native tracks on raw N-SPC banks."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_phrase import ROOT, source as phrase_source


def source():
    source = phrase_source()
    hooks = {
        '    mov $47, #$00\n': '    mov $60, #$00\n',
        'phrase_active:\n    call phrase_pointer\n    call phrase_track\n':
            'phrase_active:\n    call phrase_pointer\n    mov a, $60\n    xcn a\n    mov $47, a\n    call phrase_track\n    inc $60\n',
        'phrase_valid:\n    mov $30, #$00\n    call pair_pattern\n    ret\n':
            'phrase_valid:\n    call multi_start\n    ret\n',
        'track_emit:\n': 'track_emit:\n    mov a, $64\n    bne multi_emit_ready\n    ret\nmulti_emit_ready:\n',
    }
    for old, new in hooks.items():
        if source.count(old) != 1:
            raise ValueError('experimental multi-track hook changed')
        source = source.replace(old, new)
    for first, last in (('phrase_track:\n', 'phrase_cache:\n'), ('pair_pattern:\n', 'track_emit:\n')):
        if source.count(first) != 1 or source.count(last) != 1:
            raise ValueError('experimental multi-track routine boundary changed')
        start, end = source.index(first), source.index(last)
        if start >= end:
            raise ValueError('experimental multi-track routine order changed')
        source = source[:start] + source[end:]
    source += '\n' + (ROOT/'firmware/sgb/score_multi.asm').read_text()
    return source


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 1024:
        raise ValueError('experimental multi-track exceeds code bound')
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
    print(f'Experimental score multi: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
