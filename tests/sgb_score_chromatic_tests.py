#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measured octave playback, finite calls, independent gates and lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_chromatic import build
from build_sgb_score_callgate import build as prior_build
from build_sgb_chromatic_fixture import NOTES, CASES, bank, build as fixture_build
from check_sgb_chromatic_reference import PITCHES, observe, check_models
from check_sgb_score_chromatic_reference import align, compare, native, native_gates, validate
from check_sgb_score_gate_reference import PULSES
from sgb_score_calls_tests import CALL, called
from sgb_score_multi_tests import authored

PROBE = None
HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(case='chromatic'):
    rows = ['D,0,0,0,61,0\n']
    for i, note in enumerate(CASES[case]):
        tick, pitch = 1000+i*87500, PITCHES[note]
        for channel in (2, 3):
            for register, value in ((2, pitch & 255), (3, pitch >> 8), (4, 2), (5, 0x8F), (6, 0x6F), (7, 0xB8)):
                rows.append(f'D,0,{tick},0,{16*channel+register},{value}\n')
        rows.extend((f'D,0,{tick},0,76,12\n', f'D,0,{tick+70000},0,92,12\n'))
    return HEADER+''.join(rows)


class ChromaticTests(unittest.TestCase):
    def test_reproducible_pitch_table_and_prior_driver(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         'd9bec07d99181ca2232515a181f6e3fc7693eab6bdaf47a1d157c2d151b5b783')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),
                         'a829b6cef80c8f0f093068c6d2702e31d3c54b2671860287d759f3e32c01d22c')
        table = build()[0xF40-0x800:0xF40-0x800+29]
        self.assertEqual([table[i] | table[16+i] << 8 for i in range(13)], list(PITCHES.values()))
        for case in CASES:
            for art in (63, 127):
                self.assertLessEqual(len(bank(art, case)), 128)
                self.assertEqual(fixture_build(art, case), fixture_build(art, case))
                self.assertEqual(set(CASES[case]), set(NOTES))
        for art in (True, 63.0, 0, 64):
            with self.assertRaises(ValueError):
                bank(art)

    def test_octave_and_call_fixtures_both_articulations(self):
        for case, notes in CASES.items():
            for art in (63, 127):
                data = bank(art, case)
                report = native(PROBE, data)
                align(report, data)
                self.assertEqual([edge['pitches'] for edge in report['keyons']], [[PITCHES[note]]*2 for note in notes])
                self.assertEqual(len(native_gates(report)), 2*len(notes))
                self.assertEqual(report['end_tick'], 16*len(notes))
                self.assertTrue(all(edge['cause'] == 1 and edge['mask'] == 12 for edge in report['keyoffs']))

    def test_octave_through_every_measured_gate_profile(self):
        for tempo, art, duration in PULSES:
            first = (duration, art, *(0x80+n for n in NOTES[:7]), 0)
            second = (duration, art, *(0x80+n for n in NOTES[7:]), 0)
            data = authored((first, first, second, second))
            report = native(PROBE, data, tempo)
            align(report, data)
            self.assertEqual([edge['pitches'][0] for edge in report['keyons']], list(PITCHES.values()))

    def test_new_pitches_across_three_repeats_and_returns(self):
        for low, high in ((26, 35), (27, 34), (28, 33), (29, 32), (30, 31)):
            stream = (*CALL, 3, 0xA4, 0x98, 0)
            data = called((stream,)*4, (16, 63, 0x80+low, 0x80+high, 0))
            report = native(PROBE, data)
            align(report, data)
            self.assertEqual(len(report['events']), 32)
            expected = ([PITCHES[low], PITCHES[high]]*3+[2140, 1068])*2
            self.assertEqual([edge['pitches'][0] for edge in report['keyons']], expected)

    def test_asynchronous_new_pitches_preserve_peer_and_clip(self):
        stream, peer = (*CALL, 1, 0xA3, 0), (24, 127, 0x9C, 0xA1, 0)
        data = called((stream, peer, peer, stream), (8, 63, 0x9A, 16, 0x9F, 0), relocated=True)
        report = native(PROBE, data)
        align(report, data)
        self.assertGreater(min(report['peer_checks']), 100)
        self.assertGreater(min(report['settled_peer_nonzero_frames']), 64)
        self.assertEqual(sum(note['cause'] == 0 for note in native_gates(report)), 2)

    def test_changed_duration_and_articulation_through_return(self):
        stream = (*CALL, 2, 0x9D, 0xA0, 0)
        data = called((stream,)*4, (16, 63, 0x9B, 24, 127, 0xA2, 0))
        report = native(PROBE, data)
        align(report, data)
        self.assertEqual(report['end_tick'], 256)
        self.assertEqual([event['articulation'] for event in report['events'] if event['channel'] == 2],
                         ([63, 127]*2+[127]*2)*2)

    def test_unsupported_notes_profiles_and_truncations_are_silent(self):
        good = (16, 127, 0x98, 0)
        for opcode in (0x80, 0x97, 0xA5, 0xC8):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = (16, 127, 0x9A, opcode, 0)
                self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        for body in ((16, 127, 0x9A, 32, 0xA2, 0), (16, 64, 0x9E, 0), (0,)):
            data = called(((*CALL, 1, 0),)*4, body)
            self.assertEqual(native(PROBE, data)['status'], 0xE2)
        for case in CASES:
            data = bank(63, case)
            for size in range(1, len(data)):
                self.assertEqual(native(PROBE, data[:size])['status'], 0xE2)

    def test_reference_observer_rejects_wrong_pitch_setup_and_missing_release(self):
        data = trace()
        result = observe(io.StringIO(data+'R,0,2000000,0,10,private sentinel\n'))
        self.assertEqual(len(result['note_gates']), 26)
        self.assertEqual(result['base_notes'], list(NOTES))
        for changed in (HEADER, data.replace('D,0,1000,0,34,44\n', 'D,0,1000,0,34,45\n'),
                        data.replace(',0,39,184\n', ',0,39,183\n'), data.replace(',0,76,12\n', ',0,76,4\n'),
                        data.replace('D,0,71000,0,92,12\n', ''),
                        data.replace(f'D,0,{1000+6*87500+70000},0,92,12\n', '')):
            with self.assertRaises(ValueError):
                observe(io.StringIO(changed))

    def test_report_and_two_model_comparison_guards(self):
        candidates = {f'{case}-{art}': native(PROBE, bank(art, case)) for case in CASES for art in (127, 63)}
        refs = []
        for case in CASES:
            for art in (127, 63):
                result = observe(io.StringIO(trace(case)), case)
                result['note_gates'] = [{'voice': note['voice'], 'release_kind': 'keyoff',
                                        'gate_spc_cycles': int(note['gate_spc_cycles'])}
                                       for note in native_gates(candidates[f'{case}-{art}'])]
                refs.extend({**copy.deepcopy(result), 'model': model, 'case': case, 'articulation': art}
                            for model in ('sgb', 'sgb2'))
        self.assertEqual(len(compare(candidates, refs)), 8)
        for changed in (None, refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(candidates['chromatic-127'])
        changed['keyons'][2]['pitches'][0] += 1
        with self.assertRaises(ValueError):
            validate(changed)
        changed = copy.deepcopy(refs)
        changed[0]['note_gates'][0]['gate_spc_cycles'] += 6000
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        changed = copy.deepcopy(refs[:2])
        changed[0]['keyons'][2]['voices'][0]['pitch'] += 1
        with self.assertRaises(ValueError):
            check_models(changed)
        changed = copy.deepcopy(refs[:2])
        changed[0]['keyons'][2]['voices'][0]['pitch'] = float(PITCHES[26])
        with self.assertRaises(ValueError):
            check_models(changed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
