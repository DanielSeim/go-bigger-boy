#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack original GBS1 one-voice notes/ties/rests for resident mailbox v5."""
import argparse
import json
from pathlib import Path
import struct
from build_sgb_score_transfer import unique_object
from decode_sgb_score import decode


def build(document):
    if not isinstance(document, dict) or set(document) != {'events'}:
        raise ValueError('expected only an events array')
    events = document['events']
    if not isinstance(events, list) or not 1 <= len(events) <= 111:
        raise ValueError('expected 1..111 events')
    track = bytearray()
    for event in events:
        if not isinstance(event, dict) or len(event) != 2 or 'ticks' not in event:
            raise ValueError('event must contain ticks and exactly one of note/tie/rest')
        ticks = event['ticks']
        if type(ticks) is not int or not 1 <= ticks <= 127:
            raise ValueError('ticks must be an integer from 1 to 127')
        if 'note' in event and type(event['note']) is int and 0 <= event['note'] < 32:
            opcode = 0x80 + event['note']
        elif event.get('tie') is True:
            opcode = 0xC8
        elif event.get('rest') is True:
            opcode = 0xC9
        else:
            raise ValueError('expected note 0..31, tie true or rest true')
        track.extend((ticks, opcode))
    track.append(0)
    bank = bytearray(32)
    bank[:4] = b'GBS1'
    bank[4] = len(bank) + len(track)
    bank[8:10] = struct.pack('<H', 0x2B10)
    bank[16:18] = struct.pack('<H', 0x2B20)
    bank.extend(track)
    decode(bytes(bank), 0x2B08)  # independent grammar oracle rejects invalid ties
    payload = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return payload + bytes(4096-len(payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        document = json.loads(args.source.read_text(), object_pairs_hook=unique_object)
        payload = build(document)
        with args.output.open('xb') as output:
            output.write(payload)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
