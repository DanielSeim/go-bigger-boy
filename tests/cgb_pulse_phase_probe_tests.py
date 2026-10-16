#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline and opt-in native contracts for the authored pulse phase matrix."""
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
import cgb_pulse_phase_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_matrix_and_valid_distinct_roms(self):
        self.assertEqual(len(probe.SCENARIOS), 32)
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 64)
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

    @staticmethod
    def memory(channel):
        data = bytearray(8192)
        data[:2] = bytes((32, 1))
        data[0x200] = 0xa5
        for i in range(32):
            data[0x100 + 2 * i] = (8 if channel == 1 else 128) if i >= 12 else 0
            data[0x101 + 2 * i] = 0xf0 | (1 << (channel - 1))
        return data

    def test_channel_normalization_and_timing(self):
        a = probe.analyze(self.memory(1), 'ch1-enable-3-trigger-1', 2)
        b = probe.analyze(self.memory(2), 'ch2-enable-3-trigger-1', 2)
        self.assertTrue(a['authored_constraints_match'])
        self.assertTrue(b['authored_constraints_match'])
        self.assertEqual(a['events'], b['events'])
        self.assertEqual(a['event_sha256'], b['event_sha256'])
        self.assertEqual(a['first_nonzero_sample'], 12)
        self.assertEqual(a['trigger_after_enable_cpu_t_cycles'], 84)
        self.assertEqual(a['pcm_read_after_trigger_cpu_t_cycles'], list(range(12, 140, 4)))
        self.assertEqual((a['enable_phase_nops'], a['trigger_phase_nops']), (3, 1))

    def test_incomplete_and_damaged_records_fail_closed(self):
        for offset in (0, 1, 0x200):
            data = self.memory(1)
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'ch1-enable-0-trigger-0', 2)
        for offset, value in ((0x100, 0x80), (0x101, 0xf0), (0x101, 0xf3)):
            data = self.memory(1)
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'ch1-enable-0-trigger-0', 2)['authored_constraints_match'])
        data = self.memory(1)
        data[0x100:0x140:2] = bytes(32)
        self.assertFalse(probe.analyze(data, 'ch1-enable-0-trigger-0', 2)['authored_constraints_match'])

    def test_invalid_case_does_not_run_or_create_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'results'
            with patch.object(probe.isolated, 'run_probe') as run:
                with self.assertRaises(ValueError):
                    probe.run_probe(Path('/unused'), Path('/unused'), output, 'unknown')
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_disagreement_is_not_a_success(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))])
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        for field in ('repeatable', 'authored_constraints_match'):
            broken = dict(capture, **{field: False})
            report['rows'][0].update(match=True, captures=dict(gbb=broken, sameboy=capture))
            self.assertFalse(probe.isolated.passes(report))


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for native phase measurements')
class NativeProbeContracts(unittest.TestCase):
    def test_complete_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 64)
            self.assertFalse(report['qualification'])
            self.assertTrue(probe.isolated.passes(report))
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                self.assertEqual(row['different_event_indices'], [i for i in range(32) if a[i] != b[i]])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
                if not row['double_speed']:
                    self.assertTrue(row['match'])
                trigger = probe.split(row['scenario'])[2]
                self.assertEqual(row['captures']['sameboy']['first_nonzero_sample'],
                                 (12 if trigger & 1 else 11) if row['double_speed'] else 4)
            for row in report['rows']:
                for peer in report['rows']:
                    if row['double_speed'] == peer['double_speed'] and probe.split(row['scenario'])[2] == probe.split(peer['scenario'])[2]:
                        for name in ('gbb', 'sameboy'):
                            self.assertEqual(row['captures'][name]['events'], peer['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
