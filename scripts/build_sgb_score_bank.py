#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build separate-cache, bounded 2-KiB native score-bank playback."""
import argparse
import hashlib
from pathlib import Path
import re
from build_sgb_prototype import assemble
from build_sgb_score_envelope import source as envelope_source

ROOT = Path(__file__).resolve().parents[1]
MAX_BANK = 2048
LOG_BASE = 0x4000


def source():
    text = envelope_source()
    hooks = {
        '    mov a, $20\n    beq phrase_start_bad\n    cmp a, #$81\n    bcs phrase_start_bad\n': '    call bank_init\n',
        'phrase_begin:\n    mov $21, #$00\n': 'phrase_begin:\n    mov $21, #$00\n    mov $90, #$2b\n',
        '    mov a, $21\n    mov $43, a\n': '    mov a, $21\n    mov $43, a\n    mov a, $90\n    mov $91, a\n',
        '    mov a, $43\n    mov $21, a\n': '    mov a, $43\n    mov $21, a\n    mov a, $91\n    mov $90, a\n',
        '    mov a, $21\n    mov $4a, a\n': '    mov a, $21\n    mov $4a, a\n    mov a, $90\n    mov $92, a\n',
        '    mov a, $4a\n    mov $21, a\n': '    mov a, $4a\n    mov $21, a\n    mov a, $92\n    mov $90, a\n',
        '    mov a, $21\n    mov $7d, a\n': '    mov a, $21\n    mov $7d, a\n    mov a, $90\n    mov $94, a\n',
        '    mov a, $21\n    mov $7c, a\n': '    mov a, $21\n    mov $7c, a\n    mov a, $90\n    mov $93, a\n',
        '    mov a, $7c\n    mov $21, a\n': '    mov a, $7c\n    mov $21, a\n    mov a, $93\n    mov $90, a\n',
        '    mov a, $7d\n    mov $21, a\n': '    mov a, $7d\n    mov $21, a\n    mov a, $94\n    mov $90, a\n',
    }
    for old, new in hooks.items():
        expected = 2 if ('$43' in old) else 1
        if text.count(old) != expected:
            raise ValueError('experimental wide-bank pointer hook changed')
        text = text.replace(old, new)
    for first, last, replacement in (
            ('phrase_read:\n', 'phrase_word:\n', 'phrase_read:\n    jmp bank_read\n'),
            ('phrase_pointer:\n', 'phrase_cache:\n', 'phrase_pointer:\n    jmp bank_pointer\n')):
        if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
            raise ValueError('experimental wide-bank read boundary changed')
        text = text[:text.index(first)]+replacement+text[text.index(last):]
    # Relocate every cache read and explicit indexed store, including inherited
    # articulation, gates and mix state. Source $2B00 is never rewritten.
    text = re.sub(r'\$3[0-8][0-9a-f]{2}\b', lambda match: f'${int(match[0][1:],16)+0x1000:04x}', text)
    text = re.sub(r'(\.byte \$d5, \$00, )\$(3[0-8])\b',
                  lambda match: match[1]+f'${int(match[2],16)+0x10:02x}', text)
    return text+'\n.org $1300\n'+(ROOT/'firmware/sgb/score_bank.asm').read_text()


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental wide-bank image exceeds code/source bound')
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
    print(f'Experimental 2-KiB score bank: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
