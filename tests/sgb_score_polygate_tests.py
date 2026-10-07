#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent measured gates, surviving peers, score clipping and lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_polygate import build
from build_sgb_score_poly import build as poly_build
from build_sgb_multi_fixture import CASES, bank
from check_sgb_score_polygate_reference import align, compare, native, native_gates, observe, validate
from check_sgb_score_gate_reference import PULSES
from check_sgb_score_multi_reference import PITCHES
from sgb_score_multi_tests import authored

PROBE = None


class PolyGateTests(unittest.TestCase):
    def test_reproducible_and_source_profiles(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '15ef967662b497ad4165414bd88441ee00143c2b1ab6a49ffaf4cb9948881f18')
        self.assertEqual(hashlib.sha256(poly_build()).hexdigest(),
                         '6202e7b38f6cee051d9cc0fa2eab64642ab99d8a19470ae7afb9e76a2e88b97c')
        table = build()[0xF80-0x800:0xF80-0x800+40]
        profiles = {}
        for offset in range(0, 40, 4):
            tempo, articulation, duration, pulses = table[offset:offset+4]
            profiles[tempo, articulation, duration] = pulses
        self.assertEqual(profiles, PULSES)

    def test_all_ten_profiles_on_both_voices(self):
        for tempo, articulation, duration in PULSES:
            stream = (duration, articulation, 0x98, 0x99, 0)
            data = authored((stream,)*4)
            report = native(PROBE, data, tempo)
            align(report, data)
            self.assertEqual(len(report['keyons']), 4)
            self.assertEqual(len(report['keyoffs']), 4)
            self.assertTrue(all(edge['cause'] == 1 and edge['mask'] == 12 for edge in report['keyoffs']))

    def test_short_profiles_and_mixed_inherited_articulation(self):
        for case in CASES:
            data = bank(case, 63)
            report = native(PROBE, data)
            align(report, data)
            self.assertEqual([event['articulation'] for event in report['events']], [63]*6)
        short_then_long = (8, 63, 0x98, 16, 127, 0x99, 0xA4, 0)
        long_then_short = (8, 127, 0x99, 16, 63, 0x98, 0xA4, 0)
        data = authored((short_then_long, long_then_short, long_then_short, short_then_long), relocated=True)
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual([event['articulation'] for event in report['events']],
                         [63, 127, 127, 63, 127, 63, 127, 63, 63, 127, 63, 127])
        self.assertTrue(all(edge['cause'] == 1 for edge in report['keyoffs']))
        self.assertGreater(min(report['settled_peer_nonzero_frames']), 64)

    def test_short_asynchronous_peer_and_rests(self):
        short_then_long = (8, 63, 0x98, 16, 0x99, 0)
        long = (24, 127, 0xA4, 0)
        data = authored((short_then_long, long, long, short_then_long), relocated=True)
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual([edge['mask'] for edge in report['keyons']], [12, 4, 12, 8])
        self.assertGreater(min(report['peer_checks']), 100)
        self.assertGreater(min(report['settled_peer_nonzero_frames']), 64)
        for streams in (((127, 63, *([0xC9]*4), 0),)*4,
                        ((8, 63, 0xC9, 0x98, 0),)*4):
            data = authored(streams)
            align(native(PROBE, data), data)

    def test_different_voice_articulations_at_every_measured_tempo(self):
        for tempo in (96, 128, 192):
            short = (16, 63, 0x98, 0x99, 0)
            long = (16, 127, 0x99, 0x98, 0)
            data = authored((short, long, long, short))
            report = native(PROBE, data, tempo)
            align(report, data)
            self.assertGreater(min(report['settled_peer_nonzero_frames']), 64)
            self.assertTrue(all(edge['mask'] in (4, 8) for edge in report['keyoffs']))

    def test_expiring_voice_settles_while_peer_plays_and_clips_later(self):
        for case in CASES:
            data = bank(case)
            report = native(PROBE, data)
            align(report, data)
            gates = native_gates(report)
            if case == 'short-first':
                self.assertGreaterEqual(report['settled_gate_frames'][0], 64)
                self.assertGreaterEqual(report['settled_peer_nonzero_frames'][0], 64)
                self.assertEqual(gates[3]['cause'], 0)
            elif case == 'long-first':
                self.assertGreaterEqual(report['settled_gate_frames'][1], 64)
                self.assertGreaterEqual(report['settled_peer_nonzero_frames'][1], 64)
                self.assertEqual(gates[2]['cause'], 0)
            else:
                self.assertTrue(all(note['cause'] == 1 for note in gates))

    def test_asynchronous_rekeys_preserve_active_peer_gates(self):
        short_then_long = (8, 127, 0x98, 16, 0x99, 0)
        long = (24, 127, 0xA4, 0)
        data = authored((short_then_long, long, long, short_then_long), relocated=True)
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual([edge['mask'] for edge in report['keyons']], [12, 4, 12, 8])
        self.assertGreater(min(report['peer_checks']), 100)
        self.assertGreater(min(report['settled_gate_frames']), 64)
        self.assertEqual(report['end_tick'], 48)

    def test_rests_and_supported_event_bound(self):
        cases = (((8, 127, 0xC9, 0xC9, 0),)*4,
                 ((8, 127, 0xC9, 8, 0x98, 0), (16, 127, 0x99, 0),
                  (16, 127, 0x98, 0), (8, 127, 0xC9, 8, 0x99, 0)),
                 ((24, 127, 0x98, 0x99, 0xC9, 0xA4, 0),)*4,
                 ((127, 127, *([0xC9]*4), 0),)*4)
        for streams in cases:
            data = authored(streams)
            align(native(PROBE, data), data)

    def test_unsupported_profiles_and_ambiguous_late_banks_reject(self):
        good = (16, 127, 0x98, 0)
        for bad in ((1, 127, 0x98, 0), (7, 127, 0x98, 0), (32, 127, 0x98, 0),
                    *((16, articulation, 0x98, 0) for articulation in (0, 62, 64, 126)),
                    (16, 127, 0x80, 0), (16, 127, *([0x98]*5), 0)):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = bad
                self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        for tempo in (128, 192):
            for articulation in (63, 127):
                self.assertEqual(native(PROBE, authored(((8, articulation, 0x98, 0),)*4), tempo)['status'], 0xE2)
        for bad in ((8, 63, 0x98, 16, 64, 0x99, 0),
                    (16, 127, 0x98, 24, 63, 0x99, 32, 0xA4, 0)):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = bad
                self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        short, changed = (8, 127, 0x98, 0), (8, 127, 0x99, 0xA4, 0)
        for streams in ((short, changed, good, good), (good, good, changed, short)):
            self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        data = bank('short-first')
        for size in range(1, len(data)):
            self.assertEqual(native(PROBE, data[:size])['status'], 0xE2)
        self.assertEqual(native(PROBE, bytes(129))['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, authored((good,)*4), tempo)['status'], 0xE1)

    def test_report_and_reference_guards(self):
        candidates = {case: native(PROBE, bank(case)) for case in CASES}
        report = candidates['short-first']
        for changed in (None, {}, {**report, 'playback': True}, {**report, 'keyons': []},
                        {**report, 'keyoffs': []}, {**report, 'settled_gate_frames': [True, 0]}):
            with self.assertRaises(ValueError):
                validate(changed)
        for name, key, value in (('keyons', 'pending_pulses', [14, 15]), ('keyoffs', 'cause', 0),
                                 ('keyoffs', 'affected_mask', 4), ('keyoffs', 'half_cycle', 28000)):
            changed = copy.deepcopy(report)
            changed[name][0][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        for value in (None, True, 63.0, 62, 64):
            changed = copy.deepcopy(report)
            changed['events'][0]['articulation'] = value
            with self.assertRaises(ValueError):
                validate(changed)
        changed = copy.deepcopy(report)
        changed['events'][0]['articulation'] = 63
        with self.assertRaises(ValueError):
            align(changed, bank('short-first'))
        refs = [{'case': case, 'model': model, 'articulation': 127,
                 'keyons': [{'mask': 12, 'voices': [{'voice': voice+2, 'srcn': 2, 'pitch': pitch}
                            for voice, pitch in enumerate(pitches)]} for pitches in PITCHES],
                 'onset_intervals_spc_cycles': [43500, 87000 if case == 'both-long' else 43500],
                 'note_gates': [{'voice': note['voice'], 'gate_spc_cycles': int(note['gate_spc_cycles']),
                                 'release_kind': 'retrigger' if note['cause'] == 0 else 'keyoff'}
                                for note in native_gates(candidates[case])]}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        with self.assertRaises(ValueError):
            compare(candidates, refs[:-1])
        changed = copy.deepcopy(refs)
        changed[0]['articulation'] = 63
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['note_gates'][0]['gate_spc_cycles'] += 6000
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        header = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'
        rows = ['D,0,0,0,61,0\n']
        for tick, pitches in zip((1000, 44500, 88000), PITCHES):
            for channel, pitch in zip((2, 3), pitches):
                for register, value in ((2, pitch & 255), (3, pitch >> 8), (4, 2)):
                    rows.append(f'D,0,{tick},0,{channel*16+register},{value}\n')
            rows.extend((f'D,0,{tick},0,76,12\n', f'D,0,{tick+30000},0,92,12\n'))
        result = observe(io.StringIO(header+''.join(rows)+'R,0,99999,0,100,private sentinel\n'))
        self.assertEqual([note['gate_spc_cycles'] for note in result['note_gates']], [30000]*6)
        handoff = (header+''.join(rows)).replace('D,0,74500,0,92,12\n', 'D,0,74500,0,92,4\n')
        result = observe(io.StringIO(handoff))
        self.assertEqual(result['note_gates'][3]['release_kind'], 'retrigger')
        self.assertEqual(result['note_gates'][3]['gate_spc_cycles'], 43500)
        with self.assertRaises(ValueError):
            observe(io.StringIO((header+''.join(rows)).replace('D,0,31000,0,92,12\n', 'D,0,31000,0,92,4\n')))
        with self.assertRaises(ValueError):
            observe(io.StringIO(header+''.join(rows[:-1])))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
