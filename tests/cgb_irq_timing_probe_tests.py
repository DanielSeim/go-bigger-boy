#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned IRQ event-log contracts and optional paired reference measurements."""
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
import cgb_irq_timing_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_rom_headers_and_distinct_controls(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 12)
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

    @staticmethod
    def memory(sources=(2, 4), polled=False):
        data = bytearray(8192)
        data[:2] = bytes((32, 1))
        data[0x200] = 0xa5
        for i in range(32):
            data[0x100 + 4 * i:0x104 + 4 * i] = bytes(
                (sources[i % len(sources)], i, 2 if polled else 0, 235))
        return data

    def test_source_and_acknowledgement_constraints(self):
        self.assertTrue(probe.analyze(self.memory(), 'mixed-short', 2)['authored_constraints_match'])
        self.assertTrue(probe.analyze(self.memory((4,)), 'timer-only', 2)['authored_constraints_match'])
        self.assertFalse(probe.analyze(self.memory((4,)), 'mixed-short', 2)['authored_constraints_match'])
        self.assertTrue(probe.analyze(self.memory((2,)), 'stat-only', 2)['authored_constraints_match'])
        self.assertFalse(probe.analyze(self.memory((2,), True), 'stat-only', 2)['authored_constraints_match'])

    def test_polling_requires_latched_stat_request(self):
        self.assertTrue(probe.analyze(self.memory((2,), True), 'poll-stat', 2)['authored_constraints_match'])
        self.assertFalse(probe.analyze(self.memory((2,)), 'poll-stat', 2)['authored_constraints_match'])

    def test_incomplete_or_invalid_events(self):
        for offset in (0, 1, 0x200):
            data = self.memory()
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'mixed-short', 2)
        for offset, value in ((0x100, 8), (0x101, 144), (0x102, 32)):
            data = self.memory()
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'mixed-short', 2)['authored_constraints_match'])
        with self.assertRaises(ValueError):
            probe.analyze(bytes(512), 'mixed-short', 2)

    def test_cli_keeps_mismatches_as_failure(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))])
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for native timing measurements')
class NativeProbeContracts(unittest.TestCase):
    def test_event_logs_and_matched_cost_control(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 24)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                differences = [i for i in range(32) if a[i] != b[i]]
                self.assertEqual(row['different_event_indices'], differences)
                self.assertEqual(row['match'], not differences)
                if differences:
                    self.assertEqual(row['first_event_difference']['index'], differences[0])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
            # Completed logs freeze. Scroll/NOP paths have equal cost and should
            # not change the line period; compare within each emulator, not to an oracle.
            for speed in (False, True):
                for scenario in probe.SCENARIOS:
                    rows = [r for r in report['rows'] if r['scenario'] == scenario and r['double_speed'] == speed]
                    for name in ('gbb', 'sameboy'):
                        self.assertEqual(rows[0]['captures'][name]['events'], rows[1]['captures'][name]['events'])
                for frame in (2, 4):
                    rows = [r for r in report['rows'] if r['scenario'] in ('mixed-pad', 'mixed-scroll')
                            and r['double_speed'] == speed and r['frame'] == frame]
                    for name in ('gbb', 'sameboy'):
                        self.assertEqual(rows[0]['captures'][name]['events'], rows[1]['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
