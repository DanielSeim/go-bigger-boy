#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a bounded, explicit N-SPC data subset; no playback or title qualification."""
import argparse
import json
from pathlib import Path

BASE = 0x2B00
END = 0x4B00
MAX_OPERATIONS = 4096
MAX_EVENTS = 1024
MAX_PATTERNS = 64
CONTROLS = {0xE0: 'instrument', 0xE1: 'pan', 0xE5: 'song_volume',
            0xE7: 'tempo', 0xEA: 'transpose', 0xED: 'volume'}


def decode(bank, phrase):
    """Read only supplied score bytes, with explicit root and bounded expansion.

    This is a syntax oracle for original fixtures, not a scheduler. Track ticks
    are local positions: they do not resolve cross-channel phrase termination.
    No song-table location, instrument table or variant is inferred.
    """
    if not isinstance(bank, bytes) or not 1 <= len(bank) <= END - BASE:
        raise ValueError('bank must contain 1..8192 bytes based at $2B00')
    if type(phrase) is not int:
        raise ValueError('phrase must be an integer SPC address')
    operations = 0
    events = 0

    def read(address, size=1):
        nonlocal operations
        operations += 1
        if operations > MAX_OPERATIONS:
            raise ValueError('score operation budget exceeded')
        if address < BASE or address + size > BASE + len(bank):
            raise ValueError(f'out-of-bank read at ${address:04X} ({size} bytes)')
        return bank[address - BASE:address - BASE + size]

    def word(address):
        return int.from_bytes(read(address, 2), 'little')

    def byte(address):
        return read(address)[0]

    states = [{'duration': None, 'articulation': None, 'held_note': None}
              for _ in range(8)]
    patterns = []
    cursor = phrase
    while True:
        pattern = word(cursor)
        cursor += 2
        if pattern == 0:
            break
        if pattern < 0x100:
            raise ValueError(f'unsupported phrase control ${pattern:04X} at ${cursor - 2:04X}')
        if len(patterns) >= MAX_PATTERNS:
            raise ValueError('score pattern budget exceeded')
        pointers = read(pattern, 16)
        tracks = []
        for channel, state in enumerate(states):
            pointer = int.from_bytes(pointers[channel * 2:channel * 2 + 2], 'little')
            if pointer == 0:
                continue
            track = {'channel': channel, 'address': pointer, 'events': []}
            tick = 0
            while True:
                address = pointer
                opcode = byte(pointer)
                pointer += 1
                if opcode == 0:
                    break
                if opcode < 0x80:
                    state['duration'] = opcode
                    following = byte(pointer)
                    if following < 0x80:
                        state['articulation'] = following
                        pointer += 1
                    if byte(pointer) < 0x80:
                        raise ValueError(f'expected note or control after duration at ${pointer:04X}')
                    continue
                event = {'address': address, 'tick': tick}
                if 0x80 <= opcode <= 0xC9:
                    if state['duration'] is None:
                        raise ValueError(f'note without duration at ${address:04X}')
                    event.update(duration=state['duration'], articulation=state['articulation'])
                    if opcode < 0xC8:
                        event.update(kind='note', note=opcode - 0x80)
                        state['held_note'] = opcode - 0x80
                    elif opcode == 0xC8:
                        if state['held_note'] is None:
                            raise ValueError(f'tie without held note at ${address:04X}')
                        event.update(kind='tie', note=state['held_note'])
                    else:
                        event['kind'] = 'rest'
                        state['held_note'] = None
                    tick += state['duration']
                elif opcode in CONTROLS:
                    argument = byte(pointer)
                    pointer += 1
                    if opcode == 0xEA and argument >= 0x80:
                        argument -= 0x100
                    event.update(kind=CONTROLS[opcode], value=argument)
                else:
                    raise ValueError(f'unsupported track opcode ${opcode:02X} at ${address:04X}')
                events += 1
                if events > MAX_EVENTS:
                    raise ValueError('score event budget exceeded')
                track['events'].append(event)
            track['ticks'] = tick
            tracks.append(track)
        patterns.append({'address': pattern, 'tracks': tracks})
    return {'schema': 'gbb-nspc-subset-v1', 'qualified': False, 'playback': False,
            'base': BASE, 'size': len(bank), 'phrase': phrase,
            'patterns': patterns, 'event_count': events}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bank', type=Path, help='Raw score bank beginning at SPC $2B00')
    parser.add_argument('--phrase', required=True, type=lambda value: int(value, 0),
                        help='Explicit phrase-list SPC address, e.g. 0x2B10')
    args = parser.parse_args()
    try:
        with args.bank.open('rb') as source:
            bank = source.read(END - BASE + 1)
        result = decode(bank, args.phrase)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
