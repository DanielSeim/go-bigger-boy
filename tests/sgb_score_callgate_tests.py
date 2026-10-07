#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite-call audio, independent measured gates and repeat/return lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_callgate import build
from build_sgb_score_calls import build as calls_build
from build_sgb_score_polygate import build as gate_build
from build_sgb_calls_fixture import CASES, bank
from check_sgb_score_callgate_reference import align, compare, native, native_gates, observe, pitches, validate
from check_sgb_score_gate_reference import PULSES
from sgb_score_calls_tests import CALL, called
from sgb_score_multi_tests import authored

PROBE = None


class CallGateTests(unittest.TestCase):
    def test_reproducible_and_prior_artifacts_unchanged(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         'a829b6cef80c8f0f093068c6d2702e31d3c54b2671860287d759f3e32c01d22c')
        self.assertEqual(hashlib.sha256(calls_build()).hexdigest(),
                         'c61eb152bd76968e8420554c50eacf98bcc6aaf67c844d9cc6b7dbe35f0fdfb3')
        self.assertEqual(hashlib.sha256(gate_build()).hexdigest(),
                         '15ef967662b497ad4165414bd88441ee00143c2b1ab6a49ffaf4cb9948881f18')

    def test_repeats_returns_and_pattern_changes_both_articulations(self):
        for case, count in CASES.items():
            for articulation in (63, 127):
                data = bank(case, articulation)
                report = native(PROBE, data)
                align(report, data)
                self.assertEqual(len(report['keyons']), 2*count+4)
                self.assertEqual(len(native_gates(report)), 4*count+8)
                self.assertTrue(all(edge['mask'] == 12 and edge['cause'] == 1 for edge in report['keyoffs']))
                self.assertEqual(report['end_tick'], 16*(2*count+4))
                self.assertTrue(all(event['articulation'] == articulation for event in report['events']))

    def test_all_ten_profiles_through_eight_event_repeats(self):
        for tempo, articulation, duration in PULSES:
            stream = (*CALL, 3, 0xA4, 0x98, 0)
            data = called((stream,)*4, (duration, articulation, 0x98, 0x99, 0))
            report = native(PROBE, data, tempo)
            align(report, data)
            self.assertEqual(len(report['events']), 32)
            self.assertEqual(len(report['keyons']), 16)
            self.assertTrue(all(edge['mask'] == 12 and edge['cause'] == 1 for edge in report['keyoffs']))

    def test_inherited_and_changing_articulation_across_returns(self):
        stream = (*CALL, 3, 0xA4, 0x98, 0)
        data = called((stream,)*4, (16, 63, 0x98, 16, 127, 0x99, 0))
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual([event['articulation'] for event in report['events'] if event['channel'] == 2],
                         ([63, 127]*3+[127, 127])*2)
        for tempo in (96, 128, 192):
            short = (8, 63, 0xC9, *CALL, 2, 0xA4, 0)
            long = (8, 127, 0xC9, *CALL, 2, 0xA4, 0)
            data = called((short, long, long, short), (16, 0x98, 0x99, 0))
            report = native(PROBE, data, tempo)
            align(report, data)
            self.assertGreater(min(report['settled_peer_nonzero_frames']), 64)
            self.assertTrue(all(edge['mask'] in (4, 8) for edge in report['keyoffs']))

    def test_asynchronous_call_return_preserves_peer_and_clips_gate(self):
        stream = (*CALL, 1, 0xA4, 0)
        peer = (24, 127, 0xA4, 0xA4, 0)
        data = called((stream, peer, peer, stream), (8, 63, 0x98, 16, 0x99, 0), relocated=True)
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual(report['end_tick'], 80)
        self.assertGreater(min(report['peer_checks']), 100)
        self.assertGreater(min(report['settled_peer_nonzero_frames']), 64)
        self.assertEqual(sum(note['cause'] == 0 for note in native_gates(report)), 2)

    def test_rests_and_complete_event_tick_bounds(self):
        stream = (*CALL, 3, 0xC9, 0xC9, 0)
        data = called((stream,)*4, (127, 63, 0xC9, 0xC9, 0))
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual(report['end_tick'], 2032)
        self.assertEqual(len(report['events']), 32)
        self.assertEqual(report['pcm']['nonzero_frames'], 0)

    def test_invalid_late_expansions_and_call_grammar_reject_before_audio(self):
        good = (16, 127, 0x98, 0)
        for body in ((0,), (*CALL, 1, 0), (16, 64, 0x98, 0),
                     (16, 127, 0x98, 32, 0x99, 0), (16, 127, 0x80, 0),
                     (16, 127, 0x98, 0xE0, 2, 0), (1, 63, 0xC9, 0)):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = (*CALL, 1, 0xA4, 0)
                self.assertEqual(native(PROBE, called(streams, body))['status'], 0xE2)
        for count in (0, 4, 255):
            stream = (*CALL, count, 0)
            self.assertEqual(native(PROBE, called((stream,)*4, (16, 127, 0x98, 0)))['status'], 0xE2)
        for stream in ((*CALL, 1, *CALL, 1, 0), (*CALL, 3, 0xA4, 0x98, 0x99, 0)):
            self.assertEqual(native(PROBE, called((stream,)*4, (16, 127, 0x98, 0x99, 0)))['status'], 0xE2)
        # A call continues at the tick where its peer ends: reject in rehearsal.
        stream, peer = (*CALL, 1, 0xA4, 0), (16, 127, 0x98, 0)
        data = called((stream, peer, good, good), (16, 127, 0x99, 0))
        self.assertEqual(native(PROBE, data)['status'], 0xE2)
        for target in (0, 0x2AFF, 0x2B7F, 0x2B80, 0x2C00):
            stream = (0xEF, target & 255, target >> 8, 1, 0)
            self.assertEqual(native(PROBE, authored((stream,)*4))['status'], 0xE2)

    def test_truncations_and_tempos(self):
        data = bank('thrice', 63)
        for size in range(1, len(data)):
            self.assertEqual(native(PROBE, data[:size])['status'], 0xE2)
        self.assertEqual(native(PROBE, bytes(129))['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, data, tempo)['status'], 0xE1)

    def test_report_reference_and_observed_release_guards(self):
        candidates = {case: native(PROBE, bank(case)) for case in CASES}
        report = candidates['once']
        for name, key, value in (('keyons', 'pending_pulses', [23, 36]), ('keyoffs', 'cause', 0),
                                 ('keyoffs', 'affected_mask', 4), ('events', 'articulation', 63)):
            changed = copy.deepcopy(report)
            changed[name][0][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        refs = [{'case': case, 'model': model, 'articulation': 127,
                 'keyons': [{'mask': 12, 'voices': [{'voice': channel, 'srcn': 2, 'pitch': pitch}
                            for channel, pitch in zip((2, 3), pair)]} for pair in pitches(case)],
                 'onset_intervals_spc_cycles': [87500]*(len(pitches(case))-1),
                 'note_gates': [{'voice': note['voice'], 'gate_spc_cycles': int(note['gate_spc_cycles']),
                                 'release_kind': 'keyoff'} for note in native_gates(candidates[case])]}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        for changed in (None, refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        for key, value in (('gate_spc_cycles', 1), ('release_kind', 'retrigger')):
            changed = copy.deepcopy(refs)
            changed[0]['note_gates'][0][key] = value
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        header = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'
        rows = ['D,0,0,0,61,0\n']
        for i, pair in enumerate(pitches('once')):
            tick = 1000+i*87500
            for channel, pitch in zip((2, 3), pair):
                for register, value in ((2, pitch & 255), (3, pitch >> 8), (4, 2)):
                    rows.append(f'D,0,{tick},0,{16*channel+register},{value}\n')
            rows.extend((f'D,0,{tick},0,76,12\n', f'D,0,{tick+30000},0,92,12\n'))
        result = observe(io.StringIO(header+''.join(rows)+'R,0,999999,0,10,private sentinel\n'), 'once')
        self.assertEqual(len(result['note_gates']), 12)
        self.assertTrue(all(note['release_kind'] == 'keyoff' for note in result['note_gates']))
        for deleted in ('D,0,31000,0,92,12\n', 'D,0,293500,0,92,12\n'):
            with self.assertRaises(ValueError):
                observe(io.StringIO((header+''.join(rows)).replace(deleted, '')), 'once')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
