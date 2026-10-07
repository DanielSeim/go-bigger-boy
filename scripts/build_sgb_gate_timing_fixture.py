#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned duration/tempo gate timing expansion fixtures."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_song_selection_fixture import build_cartridge
from build_sgb_timing_fixture import timing_payload

# Explicit measurement cases; these do not expand native renderer support.
CASES = {
    'control': (96, 127, 16),
    'half-duration': (96, 127, 8),
    'long-duration': (96, 127, 24),
    'half-duration-short': (96, 63, 8),
    'long-duration-short': (96, 63, 24),
    'middle-tempo': (128, 127, 16),
    'middle-tempo-short': (128, 63, 16),
    'double-tempo-short': (192, 63, 16),
}


def score_payload(case):
    if case not in CASES:
        raise ValueError('unknown gate timing fixture')
    return timing_payload(*CASES[case])


def build(case):
    return build_cartridge(score_payload(case), (1,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=tuple(CASES), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Owned gate timing fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
