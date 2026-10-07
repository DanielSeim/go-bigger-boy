#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native bounded multi-event scheduling, inheritance and first-end rejection."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_multi import build
from build_sgb_multi_fixture import CASES, bank, build as fixture_build
from check_sgb_score_multi_reference import align, compare, native, observe, validate, PITCHES
from check_sgb_score_phrase_reference import fixture as single_fixture
from schedule_sgb_score import schedule

PROBE = None


def authored(streams, relocated=False):
    data = bytearray(64)
    phrase, tables = (48, (32, 16)) if relocated else (16, (32, 48))
    struct.pack_into('<H', data, 0, 0x2B00+phrase)
    struct.pack_into('<HHH', data, phrase, *(0x2B00+table for table in tables), 0)
    for pattern, table in enumerate(tables):
        for channel in ((3, 2) if relocated else (2, 3)):
            struct.pack_into('<H', data, table+channel*2, 0x2B00+len(data))
            data.extend(streams[pattern*2+channel-2])
    return bytes(data)


def symbolic(data):
    return schedule(data, int.from_bytes(data[:2], 'little'))


class MultiTests(unittest.TestCase):
    def test_reproducible_and_owned_fixture_bounds(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '738341ce61c29b0492795c0fbedfe8dc96c3b598c8b140f8f588b8a8137859ac')
        for case in CASES:
            self.assertLessEqual(len(bank(case)), 128)
            self.assertEqual(fixture_build(case), fixture_build(case))
            self.assertEqual(len([e for e in symbolic(bank(case))['events'] if e['kind'] == 'note']), 6)

    def test_new_and_existing_phrase_banks(self):
        for case in CASES:
            for data in (bank(case), single_fixture(case)):
                align(native(PROBE, data), data)

    def test_independent_cursors_inheritance_rests_and_relocation(self):
        streams = ((4, 127, 0x98, 7, 0xC9, 0x99, 0),
                   (6, 127, 0x99, 9, 0xA4, 3, 127, 0x98, 0),
                   (3, 127, 0xC9, 0x98, 0x99, 0),
                   (5, 127, 0xA4, 5, 0xC9, 0))
        for relocated in (False, True):
            data = authored(streams, relocated)
            for tempo in (96, 192):
                report = native(PROBE, data, tempo)
                align(report, data)
                events = report['events']
                self.assertEqual(report['end_tick'], 27)
                self.assertIn((11, 2, 7), [(e['tick'], e['channel'], e['duration']) for e in events])

    def test_maximum_events_ticks_and_equal_ends(self):
        stream = (127, 127, 0x98, 0x99, 0xC9, 0xA4, 0)
        data = authored((stream,)*4)
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual(len(report['events']), 16)
        self.assertEqual(report['end_tick'], 1016)
        streams = ((1, 127, 0x80, 0xC9, 0),)*4
        data = authored(streams)
        align(native(PROBE, data, 192), data)

    def test_ambiguous_boundary_rejects_before_any_events(self):
        short = (8, 127, 0x98, 0)
        new_event = (8, 127, 0x99, 0xA4, 0)
        second = (16, 127, 0x98, 0)
        for streams in ((short, new_event, second, second), (new_event, short, second, second),
                        (second, second, short, new_event), (second, second, new_event, short)):
            data = authored(streams)
            with self.assertRaises(ValueError):
                symbolic(data)
            self.assertEqual(native(PROBE, data)['status'], 0xE2)

    def test_invalid_late_streams_and_bounds(self):
        good = (16, 127, 0x98, 0)
        for bad in ((0,), (0x98, 0), (16, 0x98, 0), (16, 63, 0x98, 0),
                    (16, 127, 0x98, 0xEF, 0), (16, 127, 0x98, 4, 63, 0x99, 0),
                    (16, 127, *([0x98]*5), 0), (16, 127, 0x98, 4, 0, 0x99, 0)):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = bad
                self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        data = bank('short-first')
        for size in range(1, len(data)):
            self.assertEqual(native(PROBE, data[:size])['status'], 0xE2)
        self.assertEqual(native(PROBE, bytes(129))['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, data, tempo)['status'], 0xE1)

    def test_report_reference_and_sanitized_trace_guards(self):
        candidates = {case: native(PROBE, bank(case)) for case in CASES}
        report = candidates['short-first']
        for changed in (None, {}, {**report, 'playback': True}, {**report, 'events': []},
                        {**report, 'end_tick': True}):
            with self.assertRaises(ValueError):
                validate(changed)
        changed = copy.deepcopy(report)
        changed['events'][2]['duration'] = 9
        with self.assertRaises(ValueError):
            align(changed, bank('short-first'))
        keyons = [{'mask': 12, 'voices': [{'voice': voice+2, 'srcn': 2, 'pitch': pitch}
                   for voice, pitch in enumerate(pitches)]} for pitches in PITCHES]
        refs = [{'case': case, 'model': model, 'keyons': keyons,
                 'onset_intervals_spc_cycles': [43500, 87000 if case == 'both-long' else 43500]}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        for changed in (refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['onset_intervals_spc_cycles'][0] = 20000
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        header = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'
        rows = ['D,0,0,0,61,0\n']
        for tick, pitches in zip((1000, 44500, 88000), PITCHES):
            for channel, pitch in zip((2, 3), pitches):
                for register, value in ((2, pitch & 255), (3, pitch >> 8), (4, 2)):
                    rows.append(f'D,0,{tick},0,{channel*16+register},{value}\n')
            rows.append(f'D,0,{tick},0,76,12\n')
        result = observe(io.StringIO(header+''.join(rows)+'R,0,99999,0,100,private sentinel\n'))
        self.assertEqual(result['onset_intervals_spc_cycles'], [43500, 43500])
        with self.assertRaises(ValueError):
            observe(io.StringIO(header+''.join(rows[:-1])))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
