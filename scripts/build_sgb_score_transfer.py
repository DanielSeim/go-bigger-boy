#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack two authored GBB v3 motifs into a 4 KiB SOU_TRN screen payload."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SCORE_ADDRESS = 0x07D0
RESIDENT_ENTRY = 0x0200


def build(document, driver_entry=RESIDENT_ENTRY):
    """Validate the complete authoring input before creating transport bytes."""
    if not isinstance(document, dict) or set(document) != {'scores'}:
        raise ValueError('expected an object containing only "scores"')
    scores = document['scores']
    if not isinstance(scores, list) or len(scores) != 2:
        raise ValueError('scores must contain exactly two motifs')
    for score in scores:
        if not isinstance(score, list) or len(score) != 8:
            raise ValueError('each motif must contain exactly eight pitch units')
        if any(type(note) is not int or not 1 <= note <= 63 for note in score):
            raise ValueError('pitch units must be integers from 1 to 63 (DSP pitch high bytes)')
    if type(driver_entry) is not int or not 0x0100 <= driver_entry < 0xFFC0:
        raise ValueError('driver entry must be in $0100..FFBF')
    if SCORE_ADDRESS <= driver_entry < SCORE_ADDRESS + 16:
        raise ValueError('driver entry overlaps the uploaded score data')
    notes = bytes(scores[0] + scores[1])
    transfer = struct.pack('<HH', len(notes), SCORE_ADDRESS) + notes
    transfer += struct.pack('<HH', 0, driver_entry)
    return transfer + bytes(4096 - len(transfer))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='JSON object with two eight-note scores')
    parser.add_argument('--output', required=True, type=Path, help='Create raw VRAM payload; refuses to overwrite')
    parser.add_argument('--driver-entry', type=lambda value: int(value, 0), default=RESIDENT_ENTRY,
                        help='Already installed compatible driver entry (default: 0x0200)')
    args = parser.parse_args()
    try:
        document = json.loads(args.source.read_text(encoding='utf-8'), object_pairs_hook=unique_object)
        payload = build(document, args.driver_entry)
        with args.output.open('xb') as output:
            output.write(payload)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(f'Original GBB score transfer: {hashlib.sha256(payload).hexdigest()}')


if __name__ == '__main__':
    main()
