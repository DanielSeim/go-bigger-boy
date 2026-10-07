#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compose measured instrument-2 setup with owned two-voice mix playback."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_mix import source as mix_source


def source():
    text = mix_source()
    # Sanitized register observations only; directory and waveform are owned.
    for voice in (2, 3):
        for offset, old, new in ((4, 0, 2), (5, 0, 0x8F), (6, 0, 0x6F), (7, 0x7F, 0xB8)):
            before = f'    mov $f2, #${voice*16+offset:02x}\n    mov $f3, #${old:02x}\n'
            after = f'    mov $f2, #${voice*16+offset:02x}\n    mov $f3, #${new:02x}\n'
            if text.count(before) != 1:
                raise ValueError('experimental envelope setup hook changed')
            text = text.replace(before, after)
    marker = '.org $1010\n'
    if text.count(marker) != 1:
        raise ValueError('experimental envelope source directory hook changed')
    return text.replace(marker, '.org $1008\n; Owned source-2 directory slot, same authored looping BRR.\n.byte $10, $10, $10, $10\n'+marker)


def build():
    image = bytes(assemble(source(), 'spc', 0x0800))
    if len(image) > 4096:
        raise ValueError('experimental envelope image exceeds code/source bound')
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
    print(f'Experimental instrument-2 envelope: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
