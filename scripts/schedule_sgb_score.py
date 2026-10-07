#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Schedule an explicit, bounded vendor score subset in symbolic score ticks."""
import argparse
import json
from pathlib import Path

BASE = 0x2B00
MAX_BYTES = 8192
MAX_OPERATIONS = 8192
MAX_EVENTS = 1024
MAX_PATTERNS = 64
MAX_TICKS = 65536
CONTROLS = {0xE0: 'instrument', 0xE1: 'pan', 0xE5: 'song_volume',
            0xE7: 'tempo', 0xED: 'track_volume'}


def schedule(bank, phrase):
    if not isinstance(bank, bytes) or not 1 <= len(bank) <= MAX_BYTES:
        raise ValueError('bank must contain 1..8192 bytes based at $2B00')
    if type(phrase) is not int:
        raise ValueError('phrase must be an integer SPC address')
    operations = 0
    events = []
    patterns = []
    now = 0
    cursor = phrase

    def read(address, size=1):
        nonlocal operations
        operations += 1
        if operations > MAX_OPERATIONS:
            raise ValueError('score operation budget exceeded')
        if address < BASE or address+size > BASE+len(bank):
            raise ValueError('score read outside the supplied bank')
        return bank[address-BASE:address-BASE+size]

    def word(address):
        return int.from_bytes(read(address, 2), 'little')

    def emit(event):
        if len(events) >= MAX_EVENTS:
            raise ValueError('score event budget exceeded')
        events.append(event)

    def advance(track, tick):
        """Consume zero-time commands until one timed event or track end."""
        pending = []
        while True:
            address = track['pc']
            opcode = read(address)[0]
            track['pc'] += 1
            context = {'tick': tick, 'channel': track['channel'], 'address': address}
            if opcode == 0:
                call = track['call']
                if call is None:
                    return pending, True
                if not call['has_timed']:
                    raise ValueError('empty subroutine bodies are unsupported')
                if call['remaining'] > 1:
                    call['remaining'] -= 1
                    track['pc'] = call['target']
                    call['has_timed'] = False
                    pending.append({**context, 'kind': 'subroutine_repeat', 'remaining': call['remaining']})
                else:
                    track['pc'] = call['return']
                    track['call'] = None
                    pending.append({**context, 'kind': 'subroutine_return'})
                continue
            if opcode < 0x80:
                track['duration'] = opcode
                following = read(track['pc'])[0]
                if following < 0x80:
                    track['articulation'] = following
                    track['pc'] += 1
                if read(track['pc'])[0] < 0x80:
                    raise ValueError('expected a note or control after duration')
                continue
            if opcode in CONTROLS:
                value = read(track['pc'])[0]
                track['pc'] += 1
                pending.append({**context, 'kind': CONTROLS[opcode], 'value': value})
                continue
            if opcode == 0xEF:
                if track['call'] is not None:
                    raise ValueError('nested or recursive subroutine calls are unsupported')
                target = word(track['pc'])
                count = read(track['pc']+2)[0]
                track['pc'] += 3
                if count not in (1, 2, 3):
                    raise ValueError('subroutine count must be 1..3')
                track['call'] = {'target': target, 'return': track['pc'], 'remaining': count, 'has_timed': False}
                track['pc'] = target
                pending.append({**context, 'kind': 'subroutine_call', 'target': target, 'count': count})
                continue
            if not (0x80 <= opcode < 0xC8 or opcode == 0xC9):
                raise ValueError('unsupported scheduler track opcode')
            if track['duration'] is None or track['articulation'] is None:
                raise ValueError('timed event needs explicit duration and articulation in this pattern')
            end = tick+track['duration']
            if end > MAX_TICKS:
                raise ValueError('score tick budget exceeded')
            timed = {**context, 'kind': 'rest' if opcode == 0xC9 else 'note',
                     'duration': track['duration'], 'articulation': track['articulation'], 'end_tick': end}
            if opcode != 0xC9:
                timed['note'] = opcode-0x80
            if track['call'] is not None:
                track['call']['has_timed'] = True
            pending.append(timed)
            track['ready'] = end
            return pending, False

    while True:
        pattern = word(cursor)
        cursor += 2
        if pattern == 0:
            break
        if pattern < 0x100:
            raise ValueError('phrase controls and repeats are unsupported')
        if len(patterns) >= MAX_PATTERNS:
            raise ValueError('score pattern budget exceeded')
        table = read(pattern, 16)
        pointers = [int.from_bytes(table[i:i+2], 'little') for i in range(0, 16, 2)]
        channels = [i for i, pointer in enumerate(pointers) if pointer]
        if channels not in ([2], [2, 3]):
            raise ValueError('scheduler supports channel 2 alone or channels 2/3 together')
        tracks = [{'channel': channel, 'pc': pointers[channel], 'ready': now,
                   'duration': None, 'articulation': None, 'call': None} for channel in channels]
        start = now
        event_start = len(events)
        while True:
            tick = min(track['ready'] for track in tracks)
            if tick > MAX_TICKS:
                raise ValueError('score tick budget exceeded')
            pending, ended = [], []
            for track in tracks:
                if track['ready'] != tick:
                    continue
                batch, finished = advance(track, tick)
                pending.extend(batch)
                if finished:
                    ended.append(track['channel'])
            if ended:
                if tick == start:
                    raise ValueError('zero-length patterns are unsupported')
                # Same-tick end/note ordering has not been qualified against the
                # reference; do not guess whether that new note would key on.
                if any(event['kind'] in ('note', 'rest') for event in pending):
                    raise ValueError('simultaneous track end and timed event is unsupported')
                for event in pending:
                    emit(event)
                now = tick
                for event in events[event_start:]:
                    if 'end_tick' in event and event['end_tick'] > now:
                        event['end_tick'] = now
                        event['truncated'] = True
                patterns.append({'address': pattern, 'start_tick': start, 'end_tick': now,
                                 'channels': channels, 'ended_channels': ended,
                                 'truncated_channels': [channel for channel in channels if channel not in ended]})
                break
            for event in pending:
                emit(event)
    return {'schema': 'gbb-nspc-scheduler-subset-v1', 'qualification': False, 'playback': False,
            'timing_domain': 'symbolic_score_ticks', 'base': BASE, 'size': len(bank),
            'phrase': phrase, 'ticks': now, 'patterns': patterns, 'events': events}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bank', type=Path, help='Raw score bank beginning at SPC $2B00')
    parser.add_argument('--phrase', required=True, type=lambda value: int(value, 0))
    args = parser.parse_args()
    try:
        with args.bank.open('rb') as source:
            bank = source.read(MAX_BYTES+1)
        result = schedule(bank, args.phrase)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
