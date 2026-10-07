#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build gated finite-call audio for the measured chromatic octave."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_callgate import ROOT, source as call_source


def source():
    text = call_source()
    # Start the score timer after complete validation/rehearsal and DSP setup,
    # before preparing the initial notes. Avoid counting startup setup as extra
    # time after the first KON; subsequent events retain the same pulse clock.
    timer = '    mov $fa, #$10\n    mov $f1, #$81\n'
    start = '    call poly_setup\n    mov $64, #$01\n'
    if text.count(timer) != 1 or text.count(start) != 1:
        raise ValueError('experimental chromatic timer startup hook changed')
    text = text.replace(timer, '')
    text = text.replace(start, '    call poly_setup\n'+timer+'    mov $64, #$01\n')
    first, last = 'duet_voice:\n', 'duet_pitch:\n'
    if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
        raise ValueError('experimental chromatic pitch writer boundary changed')
    text = text[:text.index(first)] + (ROOT/'firmware/sgb/score_chromatic.asm').read_text()+'\n'+text[text.index(last):]
    first, last = 'duet_validate:\n', 'duet_setup:\n'
    if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
        raise ValueError('experimental chromatic note validator boundary changed')
    validator = ('duet_validate:\n    cmp a, #$c9\n    beq duet_valid\n'
                 '    cmp a, #$98\n    bcc chromatic_bad\n    cmp a, #$a5\n    bcc duet_valid\n'
                 'chromatic_bad:\n    jmp pair_reject\nduet_valid:\n    ret\n')
    text = text[:text.index(first)] + validator + text[text.index(last):]
    old = '.org $0f80\n'
    if text.count(old) != 1:
        raise ValueError('experimental chromatic pitch table hook changed')
    # Independently entered observations; parallel low/high bytes for notes 24..36.
    table = ('.org $0f40\n.byte $2c, $6c, $b0, $f8, $44, $94, $e8, $44, $a4, $08, $74, $e4, $5c\n'
             '.org $0f50\n.byte $04, $04, $04, $04, $05, $05, $05, $06, $06, $07, $07, $07, $08\n')
    return text.replace(old, table+old)


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental chromatic driver exceeds code/source bound')
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
    print(f'Experimental chromatic renderer: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
