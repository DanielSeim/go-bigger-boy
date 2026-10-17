#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""STOP/APU clock witness contracts and opt-in paired native measurements."""
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
import cgb_stop_apu_probe as probe


class Contracts(unittest.TestCase):
    def test_matrix_headers_switches_and_bounded_roms(self):
        roms = []
        for speed in (False, True):
            for scenario in probe.SCENARIOS:
                rom = probe.probe_rom(scenario, speed)
                roms.append(rom)
                _, _, operation, _ = probe.split(scenario)
                self.assertEqual(len(rom), 32768)
                self.assertEqual(rom[0x143], 0xc0)
                self.assertEqual(rom.count(bytes.fromhex('3e 01 e0 4d 10 00')),
                                 int(speed) + (255 if operation == 'switch' else 0))
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
    def memory(channel, initial_speed, switched):
        data = bytearray(8192)
        data[:4] = bytes((128, 1, initial_speed | 0x7e, initial_speed))
        data[0x400] = 0xa5
        final = initial_speed ^ (0x80 if switched else 0)
        for i in range(128):
            data[0x100 + 6 * i:0x106 + 6 * i] = bytes((
                (15 if channel == 1 else 240) if i & 1 else 0,
                0xf0 | (1 << (channel - 1)), final | 0x7e, 0, 0x30, 0xe4))
        return data

    def test_normalized_pcm_timer_and_speed_fields(self):
        for initial in (0, 0x80):
            for operation in ('switch', 'nop'):
                a = probe.analyze(self.memory(1, initial, operation == 'switch'),
                                  f'ch1-active-{operation}-period-28', 2)
                b = probe.analyze(self.memory(2, initial, operation == 'switch'),
                                  f'ch2-active-{operation}-period-28', 2)
                self.assertTrue(a['authored_constraints_match'])
                self.assertEqual(a['events'], b['events'])
                self.assertEqual(a['event_sha256'], b['event_sha256'])
                self.assertEqual(a['initial_double_speed'], bool(initial))
                self.assertEqual(a['final_double_speed'], bool(initial) ^ (operation == 'switch'))
                self.assertEqual(a['timer'][0], [0, 48, 4])
                self.assertEqual(a['timer_read_after_pcm_cpu_t_cycles'], [60, 80, 100])
                self.assertEqual(a['pcm_read_after_anchor_cpu_t_cycles'], list(range(12, 524, 4)))
        data = self.memory(1, 0, True)
        base = probe.analyze(data, 'ch1-active-switch-period-16', 2)
        data[0x105] = 4  # Unused IF high bits do not affect the comparison.
        self.assertEqual(base['event_sha256'], probe.analyze(data, 'ch1-active-switch-period-16', 2)['event_sha256'])
        data[0x104] += 1
        modified = probe.analyze(data, 'ch1-active-switch-period-16', 2)
        self.assertEqual(base['pcm_sha256'], modified['pcm_sha256'])
        self.assertNotEqual(base['timer_sha256'], modified['timer_sha256'])
        self.assertNotEqual(base['event_sha256'], modified['event_sha256'])

    def test_guards_headers_and_invalid_observations(self):
        for offset in (0, 1, 0x400):
            data = self.memory(1, 0, True)
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'ch1-active-switch-period-16', 2)
        data = self.memory(1, 0, True)
        data[2] ^= 0x80
        with self.assertRaisesRegex(ValueError, 'CPU speed'):
            probe.analyze(data, 'ch1-active-switch-period-16', 2)
        for offset, value in ((0x100, 1), (0x101, 0xf0), (0x102, 0x7e), (0x102, 0xff)):
            data = self.memory(1, 0, True)
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'ch1-active-switch-period-16', 2)['authored_constraints_match'])
        data = self.memory(1, 0, True)
        data[0x100:0x400:6] = bytes(128)
        self.assertFalse(probe.analyze(data, 'ch1-active-switch-period-16', 2)['authored_constraints_match'])

    def test_invalid_case_never_creates_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'results'
            with patch.object(probe.isolated, 'run_probe') as run:
                with self.assertRaises(ValueError):
                    probe.run_probe(Path('/unused'), Path('/unused'), output, 'unknown')
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_cli_failure_is_not_success(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))])
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set native runner and pinned SameBoy adapter for STOP clock measurements')
class NativeContracts(unittest.TestCase):
    def test_complete_clock_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 48)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][name] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['pcm_match'], a['pcm'] == b['pcm'])
                self.assertEqual(row['timer_match'], a['timer'] == b['timer'])
                self.assertEqual(row['match'], row['pcm_match'] and row['timer_match'])
                self.assertEqual(row['different_event_indices'], [i for i in range(128) if a['events'][i] != b['events'][i]])
                self.assertTrue(row['match'])
                self.assertTrue(row['pcm_match'])
                self.assertTrue(row['timer_match'])
                for capture in (a, b):
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
                    self.assertEqual(capture['initial_double_speed'], row['double_speed'])
                    self.assertEqual(capture['final_double_speed'], row['double_speed'] ^ (operation == 'switch'))
                peer = next(r for r in report['rows'] if r['double_speed'] == row['double_speed']
                            and r['scenario'] == 'ch2' + row['scenario'][3:])
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(row['captures'][name]['events'], peer['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
