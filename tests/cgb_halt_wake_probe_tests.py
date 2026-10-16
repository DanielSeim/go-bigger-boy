#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored wait/dispatch controls and optional paired native measurements."""
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
import cgb_halt_wake_probe as probe


class Contracts(unittest.TestCase):
    def test_headers_controls_and_original_halt_identity(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 88)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        for modulo in (0, 254):
            for speed in (False, True):
                self.assertEqual(probe.probe_rom(f'halt-ime1-{modulo:02x}-read-0', speed),
                                 probe.boundary.probe_rom(f'halt-{modulo:02x}-phase-0', speed))
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    @staticmethod
    def memory(case):
        wait, ime, modulo, read = probe.split(case)
        edge, div = wait == 'edge', wait.endswith('div')
        padding = read + 1 if edge else read if ime else read + 4
        mode = 'halt' if ime else 'poll'
        values = probe.boundary.expected_timer(mode, modulo, 0, padding,
                                              if_read_offset=-20 if edge else 20)
        times = probe.boundary.read_times(mode, 0, padding)
        data = bytearray(8192)
        data[:2] = bytes((8, 1))
        data[0x200] = 0xa5
        for i, (counter, flags) in enumerate(values):
            pair = (flags | 0xe0, counter) if edge else (counter, flags | 0xe0)
            last = (times[i] + 60) // 256 if div else 0x82
            data[0x100 + 4 * i:0x104 + 4 * i] = bytes((*pair, 1, last))
        return data

    def test_ime_acknowledgement_witness(self):
        for wait in ('busy', 'halt'):
            for ime in (0, 1):
                case = f'{wait}-ime{ime}-00-read-0'
                data = self.memory(case)
                self.assertTrue(probe.analyze(data, case, 2)['authored_constraints_match'])
                data[0x101] = 0xe0 | (4 if ime else 0)
                result = probe.analyze(data, case, 2)
                self.assertFalse(result['record_structure_match'])

    def test_if_edge_and_div_prediction(self):
        for phase, flag in enumerate((0, 4, 4, 4)):
            case = f'edge-ime0-00-read-{phase}'
            result = probe.analyze(self.memory(case), case, 2)
            self.assertTrue(result['timer_prediction_match'])
            self.assertEqual(result['events'][0][1], flag)
            self.assertEqual(result['predicted_if_read_t_cycles'][0], 32 + 4 * phase)
        case = 'busydiv-ime1-00-read-8'
        result = probe.analyze(self.memory(case), case, 2)
        self.assertEqual(result['events'][1], [10, 0, 1])
        self.assertEqual(result['predicted_tima_read_t_cycles'][1] + 60, 256)
        data = self.memory(case)
        data[0x107] = 0  # DIV still low in the second observation.
        result = probe.analyze(data, case, 2)
        self.assertTrue(result['record_structure_match'])
        self.assertFalse(result['timer_prediction_match'])

    def test_completion_and_guard(self):
        for offset in (0, 1, 0x200):
            data = self.memory('busy-ime1-00-read-0')
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'busy-ime1-00-read-0', 2)

    def test_relations_keep_an_internal_wait_difference(self):
        rows = [dict(scenario=s, double_speed=speed,
                     captures={name: dict(events=[[0, 4]] * 8) for name in ('gbb', 'sameboy')})
                for speed in (False, True) for s in probe.SCENARIOS]
        relations = probe.relations(dict(rows=rows))
        self.assertEqual(len(relations), 64)
        self.assertTrue(all(r['busy_vs_halt_match'] for r in relations))
        row = next(r for r in rows if r['scenario'] == 'halt-ime0-00-read-0')
        row['captures']['gbb']['events'] = [[1, 4]] + [[0, 4]] * 7
        relations = probe.relations(dict(rows=rows))
        changed = [r for r in relations if not r['busy_vs_halt_match']]
        self.assertEqual(len(changed), 1)
        self.assertEqual(changed[0]['first_event_difference']['index'], 0)

    def test_cli_preserves_cross_emulator_failure(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))],
                      wait_relations=[dict(busy_vs_halt_match=True)] * 64)
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        report['rows'][0]['match'] = True
        report['wait_relations'] = []
        self.assertFalse(probe.passes(report))


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for native wait controls')
class NativeContracts(unittest.TestCase):
    def test_repeatability_witnesses_and_truthful_phase_reporting(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 88)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                self.assertEqual(row['different_event_indices'], [i for i in range(8) if a[i] != b[i]])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['record_structure_match'])
                    self.assertEqual(capture['timer_prediction_match'], capture['events'] == capture['expected_timer'])
            self.assertEqual(len(report['wait_relations']), 64)
            self.assertTrue(all(r['busy_vs_halt_match'] for r in report['wait_relations']))
            for scenario in probe.SCENARIOS:
                rows = [r for r in report['rows'] if r['scenario'] == scenario]
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(rows[0]['captures'][name]['events'], rows[1]['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
