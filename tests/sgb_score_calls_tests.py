#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native finite-call expansion, inherited durations, bounds and lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_calls import build
from build_sgb_score_multi import build as multi_build
from build_sgb_calls_fixture import CASES, bank, build as fixture_build
from check_sgb_score_calls_reference import align, compare, native, observe, pitches, validate
from sgb_score_multi_tests import authored

PROBE = None
CALL = (0xEF, 0xFE, 0xFF)


def called(streams, body, relocated=False):
    data = bytearray(authored(streams, relocated))
    target = 0x2B00+len(data)
    data.extend(body)
    cursor = 0
    while True:
        cursor = data.find(bytes(CALL), cursor)
        if cursor < 0:
            break
        struct.pack_into('<H', data, cursor+1, target)
        cursor += 3
    return bytes(data)


class CallTests(unittest.TestCase):
    def test_reproducible_and_legacy_unchanged(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         'c61eb152bd76968e8420554c50eacf98bcc6aaf67c844d9cc6b7dbe35f0fdfb3')
        self.assertEqual(hashlib.sha256(multi_build()).hexdigest(),
                         '738341ce61c29b0492795c0fbedfe8dc96c3b598c8b140f8f588b8a8137859ac')
        for case in CASES:
            self.assertLessEqual(len(bank(case)), 128)
            self.assertEqual(fixture_build(case), fixture_build(case))

    def test_all_repetition_counts_both_channels_and_tempos(self):
        for case, count in CASES.items():
            for tempo in (96, 192):
                data = bank(case)
                if tempo == 192:
                    data = data.replace(bytes((0xE7, 96)), bytes((0xE7, 192)))
                report = native(PROBE, data, tempo)
                align(report, data)
                self.assertEqual(len(report['events']), 4*count+8)
                self.assertEqual(report['end_tick'], 16*(2*count+4))
                first = [e['opcode'] for e in report['events'] if e['channel'] == 2]
                self.assertEqual(first, [0x98, 0x99]*count+[0xA4, 0x98, 0x99, 0xA4])

    def test_caller_callee_and_return_inheritance(self):
        cases = (((8, 127, 0x98, *CALL, 2, 0xA4, 0), (16, 127, 0x99, 0), 112),
                 ((8, 127, 0x98, *CALL, 2, 0xA4, 0), (0x99, 0xC9, 0), 96),
                 ((*CALL, 3, 0xA4, 0), (16, 127, 0xC9, 0), 128),
                 ((*CALL, 2, 0xA4, 0x98, 0), (16, 127, 0x98, 8, 0x99, 0), 128))
        for stream, body, ticks in cases:
            for relocated in (False, True):
                data = called((stream,)*4, body, relocated)
                report = native(PROBE, data)
                align(report, data)
                self.assertEqual(report['end_tick'], ticks)

    def test_eight_events_per_slot_and_full_tick_bound(self):
        stream = (*CALL, 3, 0xA4, 0x98, 0)
        data = called((stream,)*4, (127, 127, 0x98, 0x99, 0))
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual(len(report['events']), 32)
        self.assertEqual(report['end_tick'], 2032)
        # Eight linear events are also supported; calls are optional.
        data = authored(((1, 127, *([0x98]*8), 0),)*4)
        align(native(PROBE, data, 192), data)

    def test_independent_call_counts_and_first_end(self):
        first = (*CALL, 1, 0xA4, 0)
        longer = (*CALL, 2, 0xA4, 0)
        data = called((first, longer, longer, first), (16, 127, 0x98, 0x99, 0))
        # At tick 48 one ends while its peer starts the fourth note: reject.
        self.assertEqual(native(PROBE, data)['status'], 0xE2)
        # Differing body/event durations permit clipping between peer events.
        second = (20, 127, 0x98, 0x99, 0xA4, 0)
        data = called((first, second, second, first), (16, 127, 0x98, 0x99, 0))
        align(native(PROBE, data), data)

    def test_invalid_calls_and_late_tracks_are_silent(self):
        good = (16, 127, 0x98, 0)
        for count in (0, 4, 255):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = (*CALL, count, 0xA4, 0)
                self.assertEqual(native(PROBE, called(streams, (16, 127, 0x98, 0)))['status'], 0xE2)
        for body in ((0,), (*CALL, 1, 0), (0xE0, 2, 16, 127, 0x98, 0),
                     (16, 63, 0x98, 0), (0x98, 0), (16, 127, *([0x98]*9), 0)):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = (*CALL, 1, 0xA4, 0)
                self.assertEqual(native(PROBE, called(streams, body))['status'], 0xE2)
        stream = (*CALL, 1, *CALL, 1, 0)
        self.assertEqual(native(PROBE, called((stream,)*4, (16, 127, 0x98, 0)))['status'], 0xE2)
        stream = (*CALL, 3, 0xA4, 0x98, 0x99, 0)
        self.assertEqual(native(PROBE, called((stream,)*4, (16, 127, 0x98, 0x99, 0)))['status'], 0xE2)
        for target in (0, 0x2AFF, 0x2B7F, 0x2B80, 0x2C00):
            stream = (0xEF, target & 255, target >> 8, 1, 0)
            self.assertEqual(native(PROBE, authored((stream,)*4))['status'], 0xE2)

    def test_truncated_banks_calls_and_runtime_limits(self):
        data = bank('thrice')
        for size in range(1, len(data)):
            self.assertEqual(native(PROBE, data[:size])['status'], 0xE2)
        for stream in ((0xEF,), (0xEF, 0), (0xEF, 0, 0x2B)):
            self.assertEqual(native(PROBE, authored(((16, 127, 0x98, 0),)*3+(stream,)))['status'], 0xE2)
        self.assertEqual(native(PROBE, bytes(129))['status'], 0xE2)
        for tempo in (0, 95, 128, 193, 255):
            self.assertEqual(native(PROBE, data, tempo)['status'], 0xE1)

    def test_report_and_reference_guards(self):
        candidates = {case: native(PROBE, bank(case)) for case in CASES}
        report = candidates['once']
        for changed in (None, {}, {**report, 'playback': True}, {**report, 'end_tick': 2033},
                        {**report, 'events': report['events']*3}):
            with self.assertRaises(ValueError):
                validate(changed)
        changed = copy.deepcopy(report)
        changed['events'][4]['opcode'] = 0x99
        with self.assertRaises(ValueError):
            align(changed, bank('once'))
        refs = [{'case': case, 'model': model,
                 'keyons': [{'mask': 12, 'voices': [{'voice': channel, 'srcn': 2, 'pitch': pitch}
                            for channel, pitch in zip((2, 3), pair)]} for pair in pitches(case)],
                 'onset_intervals_spc_cycles': [87500]*(len(pitches(case))-1)}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        for changed in (None, refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['keyons'][0]['voices'][0]['pitch'] = 2140
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['onset_intervals_spc_cycles'][0] = 84000
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        header = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'
        rows = ['D,0,0,0,61,0\n']
        for i, pair in enumerate(pitches('once')):
            tick = 1000+i*87500
            for channel, pitch in zip((2, 3), pair):
                for register, value in ((2, pitch & 255), (3, pitch >> 8), (4, 2)):
                    rows.append(f'D,0,{tick},0,{16*channel+register},{value}\n')
            rows.append(f'D,0,{tick},0,76,12\n')
        result = observe(io.StringIO(header+''.join(rows)+'R,0,999999,0,10,private sentinel\n'), 'once')
        self.assertEqual(result['onset_intervals_spc_cycles'], [87500]*5)
        with self.assertRaises(ValueError):
            observe(io.StringIO(header+''.join(rows[:-1])), 'once')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
