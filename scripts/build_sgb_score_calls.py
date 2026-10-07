#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated muted native finite calls and eight-event track caches."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_multi import ROOT, source as multi_source


def expand_cache(text):
    hooks = {
        '    xcn a\n    mov $47, a\n':
            '    xcn a\n    mov $47, a\n    clrc\n    adc a, $47\n    mov $47, a\n',
        '    adc a, #$10\n    mov $62, a\n': '    adc a, #$20\n    mov $62, a\n',
        '    cmp a, #$20\n    beq multi_finished\n    mov $30, #$20\n':
            '    cmp a, #$40\n    beq multi_finished\n    mov $30, #$40\n',
    }
    # XCN turns slot 0..3 into 0,16,32,48; doubling yields 0,32,64,96.
    for old, new in hooks.items():
        if text.count(old) != 1:
            raise ValueError('experimental native call cache hook changed')
        text = text.replace(old, new)
    return text


def source():
    text = multi_source()
    first, last = 'phrase_track:\n', 'multi_start:\n'
    if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
        raise ValueError('experimental native call parser boundary changed')
    text = text[:text.index(first)] + (ROOT/'firmware/sgb/score_calls.asm').read_text() + '\n' + text[text.index(last):]
    return expand_cache(text)


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 1024:
        raise ValueError('experimental native calls exceed code bound')
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
    print(f'Experimental native calls: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
