#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack original GBS1/GBS2/GBS3/GBS4 scores for the resident SPC renderer."""
import argparse
import json
from pathlib import Path
import struct
from build_sgb_score_transfer import unique_object
from decode_sgb_score import decode


def encode_track(events, controls_allowed):
    if not isinstance(events, list) or not 1 <= len(events) <= 111:
        raise ValueError('expected 1..111 events per track')
    track = bytearray()
    for event in events:
        if isinstance(event, dict) and len(event) == 1:
            key, value = next(iter(event.items()))
            controls = {'instrument': (0xE0, 3), 'pan': (0xE1, 20), 'volume': (0xED, 127)}
            if not controls_allowed or key not in controls:
                raise ValueError('controls require GBS2/GBS3/GBS4 and instrument/pan/volume')
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
    return track


def transport(bank):
    payload = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return payload + bytes(4096-len(payload))


def build_phrases(patterns):
    if not isinstance(patterns, list) or not 1 <= len(patterns) <= 4:
        raise ValueError('GBS4 requires 1..4 patterns')
    streams = []
    for pattern in patterns:
        if not isinstance(pattern, list) or len(pattern) != 2:
            raise ValueError('each pattern requires exactly two tracks')
        streams.append([encode_track(events, True) for events in pattern])
    size = 32 + 16*len(patterns) + sum(len(track) for pair in streams for track in pair)
    if size > 255:
        raise ValueError('combined score bank must fit 255 bytes')
    bank = bytearray(32 + 16*len(patterns))
    bank[:6] = b'GBS4' + bytes((size, len(patterns)))
    for index, pair in enumerate(streams):
        table = 32 + 16*index
        struct.pack_into('<H', bank, 8 + 2*index, 0x2B00 + table)
        for channel, track in enumerate(pair):
            struct.pack_into('<H', bank, table + 2*channel, 0x2B00 + len(bank))
            bank.extend(track)
        # GBS4 resets track state between patterns. Validate each pattern with
        # fresh oracle state as well as checking the complete finite phrase list.
        isolated = bytearray(bank)
        isolated[8:12] = struct.pack('<HH', 0x2B00 + table, 0)
        decode(bytes(isolated), 0x2B08)
    decode(bytes(bank), 0x2B08)
    return transport(bank)


def build(document):
    if not isinstance(document, dict):
        raise ValueError('expected score object')
    format_name = document.get('format', 'GBS1')
    if format_name not in ('GBS1', 'GBS2', 'GBS3', 'GBS4'):
        raise ValueError('format must be GBS1, GBS2, GBS3 or GBS4')
    if format_name == 'GBS4':
        if set(document) != {'format', 'patterns'}:
            raise ValueError('GBS4 requires format and patterns')
        return build_phrases(document['patterns'])
    if format_name == 'GBS3':
        if set(document) != {'format', 'tracks'} or not isinstance(document['tracks'], list) or len(document['tracks']) != 2:
            raise ValueError('GBS3 requires exactly two tracks')
        tracks = [encode_track(events, True) for events in document['tracks']]
    else:
        if set(document) not in ({'events'}, {'format', 'events'}):
            raise ValueError('expected events and optional format')
        tracks = [encode_track(document['events'], format_name == 'GBS2')]
    size = 32 + sum(map(len, tracks))
    if size > 255:
        raise ValueError('combined score bank must fit 255 bytes')
    bank = bytearray(32)
    bank[:4] = format_name.encode('ascii')
    bank[4] = size
    bank[8:10] = struct.pack('<H', 0x2B10)
    bank[16:18] = struct.pack('<H', 0x2B20)
    if format_name == 'GBS3':
        split = 32 + len(tracks[0])
        bank[5] = split
        bank[18:20] = struct.pack('<H', 0x2B00 + split)
    for track in tracks:
        bank.extend(track)
    decode(bytes(bank), 0x2B08)  # independent grammar oracle rejects invalid ties
    return transport(bank)


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
