#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack original GBS1/GBS2 scores for the resident SPC renderer."""
import argparse
import json
from pathlib import Path
import struct
from build_sgb_score_transfer import unique_object
from decode_sgb_score import decode


def build(document):
    if not isinstance(document, dict) or set(document) not in ({'events'}, {'format', 'events'}):
        raise ValueError('expected events and optional format')
    format_name = document.get('format', 'GBS1')
    if format_name not in ('GBS1', 'GBS2'):
        raise ValueError('format must be GBS1 or GBS2')
    events = document['events']
    if not isinstance(events, list) or not 1 <= len(events) <= 111:
        raise ValueError('expected 1..111 events')
    track = bytearray()
    for event in events:
        if isinstance(event, dict) and len(event) == 1:
            key, value = next(iter(event.items()))
            controls = {'instrument': (0xE0, 3), 'pan': (0xE1, 20), 'volume': (0xED, 127)}
            if format_name != 'GBS2' or key not in controls:
                raise ValueError('controls require GBS2 and instrument/pan/volume')
            opcode, maximum = controls[key]
            if type(value) is not int or not 0 <= value <= maximum:
                raise ValueError(f'{key} must be an integer from 0 to {maximum}')
            track.extend((opcode, value))
            continue
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
    bank[:4] = format_name.encode('ascii')
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
