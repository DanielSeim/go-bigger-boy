#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Contracts for independently authored frequency-write boundary observations."""
import contextlib
import io
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_pulse_frequency_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_matrix_headers_and_distinct_controls(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 12)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x143], 0xc0)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    def test_original_roms_remain_byte_identical(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-pulse-frequency.json').read_text())
        for row in evidence['authored']['rows']:
            rom = probe.probe_rom(row['scenario'], row['double_speed'])
            self.assertEqual(hashlib.sha256(rom).hexdigest(), row['rom_sha256'])

    def test_lifecycle_controls_and_switch_counts(self):
        self.assertEqual(len(probe.LIFECYCLE_SCENARIOS), 24)
        roms = []
        switch = bytes.fromhex('3e 01 e0 4d 10 00')
        for speed in (False, True):
            for scenario in probe.LIFECYCLE_SCENARIOS:
                rom = probe.probe_rom(scenario, speed)
                roms.append(rom)
                mode = probe.lifecycle_mode(scenario)
                self.assertEqual(rom.count(switch), int(speed) + (127 if 'switch' in mode else 0))
                self.assertEqual(len(rom), 32768)
                checksum = 0
                for value in rom[0x134:0x14d]:
                    checksum = (checksum - value - 1) & 255
                self.assertEqual(rom[0x14d], checksum)
                self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                                 sum(rom[:0x14e] + rom[0x150:]) & 65535)
        self.assertEqual(len(set(roms)), 48)

    @staticmethod
    def lifecycle_memory(channel, initial_speed, switched):
        data = bytearray(8192)
        data[:4] = bytes((64, 1, initial_speed | 0x7e, initial_speed))
        data[0x200] = 0xa5
        speed = initial_speed ^ (0x80 if switched else 0)
        for i in range(64):
            data[0x100 + 4 * i:0x104 + 4 * i] = bytes((
                0, (15 if channel == 1 else 240) if i & 1 else 0,
                0xf0 | (1 << (channel - 1)), speed | 0x7e))
        return data

    def test_lifecycle_speed_witness_and_timing_anchor(self):
        for initial in (0, 0x80):
            for mode in ('retrigger', 'switch', 'switch-retrigger'):
                data = self.lifecycle_memory(1, initial, 'switch' in mode)
                result = probe.analyze(data, 'ch1-high-' + mode, 2)
                self.assertTrue(result['authored_constraints_match'])
                self.assertEqual(result['initial_double_speed'], bool(initial))
                self.assertEqual(result['final_double_speed'], bool(initial) ^ ('switch' in mode))
                self.assertNotIn('write_after_trigger_cpu_t_cycles', result)
                self.assertEqual(result['operation_after_anchor_cpu_t_cycles'], list(range(20, 276, 4)))
                self.assertEqual(result['observation_fields'][-1], 'key1')
                data[0x103] ^= 0x80
                self.assertFalse(probe.analyze(data, 'ch1-high-' + mode, 2)['authored_constraints_match'])
                data[2] ^= 0x80
                with self.assertRaisesRegex(ValueError, 'initial CPU speed'):
                    probe.analyze(data, 'ch1-high-' + mode, 2)
        result = probe.analyze(self.lifecycle_memory(2, 0, False), 'ch2-observe-retrigger', 2)
        self.assertEqual(result['operation'], 'equal_cost_nops')

    @staticmethod
    def memory(channel):
        data = bytearray(8192)
        data[:2] = bytes((64, 1))
        data[0x200] = 0xa5
        for i in range(64):
            data[0x100 + 3 * i:0x103 + 3 * i] = bytes((
                0, (15 if channel == 1 else 240) if i & 1 else 0,
                0xf0 | (1 << (channel - 1))))
        return data

    def test_channel_normalization_and_timestamps(self):
        a = probe.analyze(self.memory(1), 'ch1-high', 2)
        b = probe.analyze(self.memory(2), 'ch2-high', 2)
        self.assertTrue(a['authored_constraints_match'])
        self.assertTrue(b['authored_constraints_match'])
        self.assertEqual(a['events'], b['events'])
        self.assertEqual(a['event_sha256'], b['event_sha256'])
        self.assertEqual(a['events'][:2], [[0, 0], [0, 15]])
        self.assertEqual(a['write_after_trigger_cpu_t_cycles'], list(range(20, 276, 4)))
        self.assertEqual(a['pcm_read_after_write_cpu_t_cycles'], [12, 96])

    def test_guards_and_invalid_status_or_amplitude(self):
        for offset in (0, 1, 0x200):
            data = self.memory(1)
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'ch1-high', 2)
        for offset, value in ((0x100, 240), (0x101, 1), (0x102, 0xf0), (0x102, 0xf3)):
            data = self.memory(1)
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'ch1-high', 2)['authored_constraints_match'])
        data = self.memory(1)
        data[0x101:0x1c0:3] = bytes(64)
        self.assertFalse(probe.analyze(data, 'ch1-high', 2)['authored_constraints_match'])

    def test_invalid_case_does_not_execute(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'results'
            with patch.object(probe.isolated, 'run_probe') as run:
                with self.assertRaises(ValueError):
                    probe.run_probe(Path('/unused'), Path('/unused'), output, 'unknown')
                with self.assertRaises(ValueError):
                    probe.run_probe(Path('/unused'), Path('/unused'), output, 'ch1-high-retrigger')
                with self.assertRaises(ValueError):
                    probe.run_probe(Path('/unused'), Path('/unused'), output, 'ch1-high', lifecycle=True)
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_cli_disagreement_fails(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))])
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        with patch.object(probe, 'run_probe', return_value=report) as run, \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused', '--lifecycle',
                    '--case', 'ch1-observe-retrigger']), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        self.assertTrue(run.call_args.kwargs['lifecycle'])
        self.assertEqual(run.call_args.args[3], 'ch1-observe-retrigger')


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for native measurements')
class NativeProbeContracts(unittest.TestCase):
    def test_lifecycle_results_preserve_disagreements(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results', lifecycle=True)
            self.assertEqual(len(report['rows']), 48)
            self.assertEqual(report['schema'], 'gbb-cgb-pulse-frequency-lifecycle-probe-v1')
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                self.assertEqual(row['different_event_indices'], [i for i in range(64) if a[i] != b[i]])
                if probe.lifecycle_mode(row['scenario']) == 'retrigger':
                    self.assertTrue(row['match'])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
                    self.assertEqual(capture['initial_double_speed'], row['double_speed'])
                    self.assertEqual(capture['final_double_speed'],
                                     row['double_speed'] ^ ('switch' in capture['lifecycle']))
                peer = next(r for r in report['rows'] if r['double_speed'] == row['double_speed']
                            and r['scenario'] == 'ch2' + row['scenario'][3:])
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(row['captures'][name]['events'], peer['captures'][name]['events'])

    def test_complete_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 12)
            self.assertFalse(report['qualification'])
            self.assertTrue(probe.isolated.passes(report))
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                self.assertEqual(row['different_event_indices'], [i for i in range(64) if a[i] != b[i]])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
            for row in report['rows']:
                peer = next(r for r in report['rows'] if r['double_speed'] == row['double_speed']
                            and r['scenario'] == 'ch2-' + probe.split(row['scenario'])[1])
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(row['captures'][name]['events'], peer['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
