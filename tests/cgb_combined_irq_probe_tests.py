#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored combined-IRQ contracts; external demo assets are never required."""
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
import cgb_combined_irq_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_rom_headers_and_distinct_controls(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 8)
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
    def memory(channel='pulse', amplitude=15, count=0):
        data = bytearray(8192)
        data[:3] = bytes((64, 1, count))
        data[0x200] = 0xa5
        data[0x100 + (channel == 'wave')] = amplitude
        return data

    def test_counter_and_channel_constraints(self):
        for channel, low in (('pulse', 5), ('wave', 7)):
            data = self.memory(channel)
            self.assertTrue(probe.analyze(data, 'combined-' + channel, 4)['authored_constraints_match'])
            for frame, amplitude in ((2, low), (3, 15), (4, low)):
                data = self.memory(channel, amplitude, frame - 1)
                result = probe.analyze(data, 'vblank-' + channel, frame)
                self.assertTrue(result['authored_constraints_match'])
                self.assertEqual(result['expected_amplitude'], amplitude)
                data[2] = 0  # Missing VBlank handoff must not be accepted.
                self.assertFalse(probe.analyze(data, 'vblank-' + channel, frame)['authored_constraints_match'])

    def test_stale_volume_and_wrong_channel_are_rejected(self):
        self.assertFalse(probe.analyze(self.memory(amplitude=15, count=1),
                                       'vblank-pulse', 2)['authored_constraints_match'])
        self.assertFalse(probe.analyze(self.memory(channel='wave'),
                                       'combined-pulse', 2)['authored_constraints_match'])

    def test_guard_and_completion(self):
        for offset in (0, 1, 0x200):
            data = self.memory()
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'combined-pulse', 2)
        with self.assertRaises(ValueError):
            probe.analyze(bytes(512), 'combined-pulse', 2)

    def test_cli_preserves_reported_mismatch_as_failure(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, pcm_match=True,
            pixel_match=False, captures=dict(gbb=capture, sameboy=capture))])
        output = io.StringIO()
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(output):
            self.assertEqual(probe.main(), 1)
        self.assertIn('"qualification": false', output.getvalue())


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for paired native probes')
class NativeProbeContracts(unittest.TestCase):
    def test_handoffs_repeatability_and_truthful_comparison(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 24)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
                    self.assertEqual(capture['sample_fields'], ['pcm12', 'pcm34', 'ly', 'tima'])
                self.assertEqual(row['match'], row['pcm_match'] and row['pixel_match'])
                self.assertEqual(row['pcm_match'], not row['different_sample_indices'])
                a = row['captures']['gbb']['samples']
                b = row['captures']['sameboy']['samples']
                self.assertEqual(row['different_timer_phase_indices'],
                                 [i for i in range(64) if a[i][3] != b[i][3]])
                if row['first_sample_difference'] is not None:
                    self.assertEqual(row['first_sample_difference']['index'], row['different_sample_indices'][0])
                self.assertEqual(row['pixel_match'], row['different_pixel_count'] == 0)
                if row['first_pixel_difference'] is not None:
                    self.assertLess(row['first_pixel_difference']['x'], 160)
                    self.assertLess(row['first_pixel_difference']['y'], 144)
                # Current mismatches remain research results; a future core fix may match.
                # The test deliberately does not require either emulator to stay wrong.
            for scenario in ('combined-pulse', 'combined-wave'):
                for speed in (False, True):
                    rows = [r for r in report['rows'] if r['scenario'] == scenario
                            and r['double_speed'] == speed]
                    for emulator in ('gbb', 'sameboy'):
                        first = rows[0]['captures'][emulator]['samples']
                        self.assertTrue(all(r['captures'][emulator]['samples'] == first for r in rows))


if __name__ == '__main__':
    unittest.main()
