#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect canonical original GBS1..GBS6 uploads; no emulation or title qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from build_sgb_resident_score import build
from decode_sgb_score import decode


def inspect(payload):
    """Report score-relative ticks, resetting track state at each phrase/pass.

    Require an exact round trip through the strict authoring packer. This tool
    accepts its canonical transport, not every stream the firmware can execute.
    """
    if not isinstance(payload, bytes) or len(payload) != 4096:
        raise ValueError('expected exactly 4096 upload bytes')
    size, address = struct.unpack_from('<HH', payload)
    if address != 0x2B00 or not 35 <= size <= 255:
        raise ValueError('expected a resident bank of 35..255 bytes at $2B00')
    bank = payload[4:4+size]
    formats = [f'GBS{number}' for number in range(1, 7)]
    signature = bank[:4]
    if signature not in [name.encode('ascii') for name in formats]:
        raise ValueError('unsupported original resident format')
    format_name = signature.decode('ascii')
    number = int(format_name[-1])
    decoded = decode(bank, 0x2B08)['patterns']
    patterns = []
    for pattern in decoded:
        # The general grammar oracle carries held-note state across patterns.
        # Resident envelopes require fresh state, including ties, every time.
        isolated = bytearray(bank)
        struct.pack_into('<HH', isolated, 8, pattern['address'], 0)
        patterns.append(decode(bytes(isolated), 0x2B08)['patterns'][0])

    def authored(track):
        result = []
        for event in track['events']:
            kind = event['kind']
            if kind in ('note', 'tie', 'rest'):
                if event['articulation'] is not None:
                    raise ValueError('resident articulation is unsupported')
                result.append({'ticks': event['duration'],
                               kind: event['note'] if kind == 'note' else True})
            else:
                result.append({kind: event['value']})
        return result

    tracks = [[authored(track) for track in pattern['tracks']] for pattern in patterns]
    document = {'format': format_name}
    if number >= 4:
        document['patterns'] = tracks
        if number >= 5:
            document['plays'] = bank[6]
    elif number == 3:
        if len(tracks) != 1:
            raise ValueError('GBS3 requires one pattern')
        document['tracks'] = tracks[0]
    else:
        if len(tracks) != 1 or len(tracks[0]) != 1:
            raise ValueError('GBS1/GBS2 require one track')
        document['events'] = tracks[0][0]
    if build(document) != payload:
        raise ValueError('upload is not a canonical resident-score transport')

    timeline = []
    tick = 0
    plays = document.get('plays', 1)
    for play in range(plays):
        for index, pattern in enumerate(patterns):
            rendered_tracks = []
            duration = max(track['ticks'] for track in pattern['tracks'])
            for track in pattern['tracks']:
                state = {'instrument': 1, 'pan': 10, 'volume': 80, 'transpose': 0}
                held = None
                events = []
                for source in track['events']:
                    event = {'tick': tick + source['tick'], 'kind': source['kind'],
                             'address': source['address']}
                    kind = source['kind']
                    if kind in ('note', 'tie', 'rest'):
                        event['ticks'] = source['duration']
                        if kind == 'note':
                            held = source['note'] + state['transpose']
                            event['base_note'] = source['note']
                        elif kind == 'rest':
                            held = None
                        event['effective_note'] = held
                        event['controls'] = dict(state)
                    else:
                        state[kind] = source['value']
                        event['value'] = source['value']
                    events.append(event)
                rendered_tracks.append({'channel': track['channel'],
                                        'dsp_voice': 4 - track['channel'],
                                        'end_tick': tick + track['ticks'], 'events': events})
            timeline.append({'play': play + 1, 'pattern': index,
                             'start_tick': tick, 'end_tick': tick + duration,
                             'tracks': rendered_tracks})
            tick += duration
    return {'schema': 'gbb-resident-score-inspection-v1', 'format': format_name,
            'qualified': False, 'playback': False,
            'upload_sha256': hashlib.sha256(payload).hexdigest(),
            'bank_bytes': size, 'mailbox_version': 4 + number,
            'tick_microseconds': 16000, 'plays': plays,
            'total_ticks': tick, 'nominal_microseconds': tick * 16000,
            'timeline': timeline}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('upload', type=Path, help='4096-byte output of build_sgb_resident_score.py')
    args = parser.parse_args()
    try:
        with args.upload.open('rb') as source:
            result = inspect(source.read(4097))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
