#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Contracts for isolated pulse retrigger clocks and equal-cost controls."""
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
import cgb_pulse_retrigger_probe as probe


class Contracts(unittest.TestCase):
    def test_distinct_roms_headers_and_no_measured_stop(self):
        roms = []
        for speed in (False, True):
            for scenario in probe.SCENARIOS:
                rom = probe.probe_rom(scenario, speed)
                roms.append(rom)
                channel, operation, _ = probe.split(scenario)
                self.assertEqual(len(rom), 32768)
                self.assertEqual(rom[0x143], 0xc0)
                self.assertEqual(rom.count(bytes.fromhex('3e 01 e0 4d 10 00')), int(speed))
                trigger = bytes((0x3e, 0x87, 0xe0, 0x14 if channel == 1 else 0x19))
                self.assertEqual(rom.count(trigger), 128 if operation == 'retrigger' else 64)
                checksum = 0
                for value in rom[0x134:0x14d]:
                    checksum = (checksum - value - 1) & 255
                self.assertEqual(rom[0x14d], checksum)
                self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                                 sum(rom[:0x14e] + rom[0x150:]) & 65535)
        self.assertEqual(len(set(roms)), 48)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    @staticmethod
    def memory(channel, speed=False):
        data = bytearray(8192)
        initial = 0x80 if speed else 0
        data[:4] = bytes((64, 1, initial | 0x7e, initial))
        data[0x200] = 0xa5
        for i in range(64):
            data[0x100 + 3 * i:0x103 + 3 * i] = bytes((
                (15 if channel == 1 else 240) if i >= 30 else 0,
                0xf0 | (1 << (channel - 1)), initial | 0x7e))
        return data

    def test_normalized_pcm_speed_and_timing(self):
        a = probe.analyze(self.memory(1, True), 'ch1-retrigger-gap-3', 2)
        b = probe.analyze(self.memory(2, True), 'ch2-retrigger-gap-3', 2)
        self.assertTrue(a['authored_constraints_match'])
        self.assertTrue(b['authored_constraints_match'])
        self.assertEqual(a['events'], b['events'])
        self.assertEqual(a['event_sha256'], b['event_sha256'])
        self.assertTrue(a['double_speed'])
        self.assertEqual(a['first_nonzero_sample'], 30)
        self.assertEqual(a['operation_after_first_trigger_cpu_t_cycles'], 32)
        self.assertEqual(a['pcm_read_after_operation_cpu_t_cycles'], list(range(12, 268, 4)))
        self.assertEqual(probe.analyze(self.memory(1), 'ch1-observe-gap-8', 2)['operation'], 'observe')

    def test_invalid_guards_speed_and_observations(self):
        for offset in (0, 1, 0x200):
            data = self.memory(1)
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'ch1-retrigger-gap-0', 2)
        data = self.memory(1)
        data[2] ^= 0x80
        with self.assertRaisesRegex(ValueError, 'CPU speed'):
            probe.analyze(data, 'ch1-retrigger-gap-0', 2)
        for offset, value in ((0x100, 1), (0x101, 0xf0), (0x102, 0xfe), (0x102, 0x7f)):
            data = self.memory(1)
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'ch1-retrigger-gap-0', 2)['authored_constraints_match'])
        data = self.memory(1)
        data[0x100:0x1c0:3] = bytes(64)
        self.assertFalse(probe.analyze(data, 'ch1-retrigger-gap-0', 2)['authored_constraints_match'])

    def test_invalid_case_does_not_execute(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'results'
            with patch.object(probe.isolated, 'run_probe') as run:
                with self.assertRaises(ValueError):
                    probe.run_probe(Path('/unused'), Path('/unused'), output, 'unknown')
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_cli_preserves_disagreement(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))])
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set native runner and pinned SameBoy adapter for retrigger measurements')
class NativeContracts(unittest.TestCase):
    def test_complete_retrigger_and_control_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 48)
            self.assertFalse(report['qualification'])
            self.assertTrue(probe.isolated.passes(report))
            for row in report['rows']:
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
                    self.assertEqual(capture['double_speed'], row['double_speed'])
                peer = next(r for r in report['rows'] if r['double_speed'] == row['double_speed']
                            and r['scenario'] == 'ch2' + row['scenario'][3:])
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(row['captures'][name]['events'], peer['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
