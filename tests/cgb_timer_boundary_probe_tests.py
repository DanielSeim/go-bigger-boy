#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned timer-boundary model and comparison contracts; no demo assets required."""
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_timer_boundary_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_headers_and_distinct_controls(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 48)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    def test_known_clock_sequences(self):
        # Enable at DIV=20; overflow at 32, reload at 36. Reads at 48+80*n.
        self.assertEqual(probe.expected_timer('poll', 0, 0), [[1 + 5 * i, 4] for i in range(8)])
        # Enable exactly at DIV=32 misses that edge; overflow is at 48 instead.
        self.assertEqual(probe.expected_timer('poll', 0, 3), [[1 + 5 * i, 4] for i in range(8)])
        self.assertEqual(probe.read_times('halt', 0), [84 + 80 * i for i in range(8)])
        # FE reloads every 32 ticks: reads alternate FF and reload-cycle FE.
        self.assertEqual(probe.expected_timer('halt', 254, 0),
                         [[255 if i % 2 == 0 else 254, 4] for i in range(8)])

    def test_independent_matrix_coverage_and_coupled_equivalence(self):
        self.assertEqual(len(probe.INDEPENDENT_SCENARIOS), 48)
        self.assertEqual(len({probe.probe_rom(s, speed) for speed in (False, True)
                              for s in probe.INDEPENDENT_SCENARIOS}), 96)
        for modulo in (0, 226, 254):
            for phase in range(4):
                for speed in (False, True):
                    legacy = probe.probe_rom(f'poll-{modulo:02x}-phase-{phase}', speed)
                    independent = probe.probe_rom(f'poll-{modulo:02x}-start-{phase}-read-{phase}', speed)
                    self.assertEqual(legacy, independent)

    def test_independent_padding_hits_reload_without_halt(self):
        self.assertEqual(probe.read_times('poll', 0, 1), [52 + 80 * i for i in range(8)])
        self.assertEqual(probe.read_times('poll', 2, 3), [68 + 80 * i for i in range(8)])
        self.assertEqual(probe.expected_timer('poll', 254, 0, 1),
                         [[255 if i % 2 == 0 else 254, 4] for i in range(8)])
        self.assertEqual(probe.expected_timer('poll', 254, 2, 3),
                         [[254 if i % 2 == 0 else 255, 4] for i in range(8)])
        data = self.memory(probe.expected_timer('poll', 254, 0, 1))
        result = probe.analyze(data, 'poll-fe-start-0-read-1', 2)
        self.assertEqual((result['start_phase_nops'], result['read_phase_nops']), (0, 1))
        self.assertIn('IME_off; IE_zero', result['prediction_assumptions'])
        self.assertNotIn('HALT', result['prediction_assumptions'])

    def test_case_validation_fails_before_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'results'
            with self.assertRaisesRegex(ValueError, 'selected phase matrix'):
                probe.run_probe(Path('/unused'), Path('/unused'), output,
                                independent_phases=True, case='halt-fe-phase-0')
            self.assertFalse(output.exists())

    @staticmethod
    def memory(values):
        data = bytearray(8192)
        data[:2] = bytes((8, 1))
        data[0x200] = 0xa5
        for i, (counter, flags) in enumerate(values):
            data[0x100 + 4 * i:0x104 + 4 * i] = bytes((counter, flags | 0xe0, 1, 0x82))
        return data

    def test_sampling_disagreement_is_not_accepted(self):
        values = probe.expected_timer('halt', 254, 0)
        result = probe.analyze(self.memory(values), 'halt-fe-phase-0', 2)
        self.assertTrue(result['record_structure_match'])
        self.assertTrue(result['timer_oracle_match'])
        values[1][0] = 0
        result = probe.analyze(self.memory(values), 'halt-fe-phase-0', 2)
        self.assertTrue(result['record_structure_match'])
        self.assertFalse(result['timer_oracle_match'])
        self.assertFalse(result['authored_constraints_match'])

    def test_guard_and_completion(self):
        for offset in (0, 1, 0x200):
            data = self.memory(probe.expected_timer('poll', 0, 0))
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'poll-00-phase-0', 2)

    def test_cli_preserves_research_failure(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))])
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        with patch.object(probe, 'run_probe', return_value=report) as run, \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused',
                    '--independent-phases', '--case', 'poll-fe-start-0-read-1']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        self.assertTrue(run.call_args.kwargs['independent_phases'])
        self.assertEqual(run.call_args.kwargs['case'], 'poll-fe-start-0-read-1')


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for native boundary measurements')
class NativeProbeContracts(unittest.TestCase):
    def test_uncoupled_foreground_collision_and_control(self):
        with tempfile.TemporaryDirectory() as tmp:
            for case in ('poll-fe-start-0-read-1', 'poll-fe-start-2-read-3', 'poll-00-start-0-read-1'):
                report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                         Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                         Path(tmp) / case, independent_phases=True, case=case)
                self.assertEqual(report['schema'], 'gbb-cgb-timer-independent-phase-probe-v1')
                self.assertEqual(len(report['rows']), 2)
                self.assertFalse(report['qualification'])
                for row in report['rows']:
                    a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                    self.assertEqual(row['match'], a == b)
                    self.assertEqual(row['different_event_indices'], [i for i in range(8) if a[i] != b[i]])
                    for capture in row['captures'].values():
                        self.assertTrue(capture['repeatable'])
                        self.assertTrue(capture['record_structure_match'])
                        self.assertIn('IME_off; IE_zero', capture['prediction_assumptions'])
                        self.assertEqual(capture['timer_oracle_match'], capture['events'] == capture['expected_timer'])
                        if case.startswith('poll-00'):
                            self.assertTrue(capture['timer_oracle_match'])
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(report['rows'][0]['captures'][name]['events'],
                                     report['rows'][1]['captures'][name]['events'])

    def test_reads_repeat_and_differences_remain_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 48)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                self.assertEqual(row['different_event_indices'], [i for i in range(8) if a[i] != b[i]])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['record_structure_match'])
                    self.assertEqual(capture['timer_oracle_match'], capture['events'] == capture['expected_timer'])
                    if row['scenario'].startswith('poll'):
                        self.assertTrue(capture['timer_oracle_match'])
            for scenario in probe.SCENARIOS:
                rows = [r for r in report['rows'] if r['scenario'] == scenario]
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(rows[0]['captures'][name]['events'], rows[1]['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
