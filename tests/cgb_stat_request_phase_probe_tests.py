# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_stat_request_phase_probe as probe


class StatRequestPhaseContracts(unittest.TestCase):
    def test_authored_roms_have_valid_headers_and_distinct_controls(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 86)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], 0xc0)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
            self.assertEqual(rom[0x1400], 0xf3)
        for bad in ('if-phase-13', 'software-phase-4', 'unknown'):
            with self.assertRaises(ValueError):
                probe.probe_rom(bad)

    @staticmethod
    def memory(scenario, speed=False):
        data = bytearray(8192)
        data[:2] = b'\x01\x01'
        data[0x3f] = 0xa5
        if scenario.startswith('software'):
            sample = [0, 0, 0, 3, 128 if speed else 0, 0x5a]
        else:
            sample = [0, 2, 2, 1, 3, 128 if speed else 0]
        data[0x10:0x16] = bytes(sample)
        return data

    def test_incomplete_and_invalid_snapshots_fail_closed(self):
        for offset in (0, 1, 0x3f):
            data = self.memory('if-phase-0')
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'if-phase-0', 4)
        with self.assertRaises(ValueError):
            probe.analyze(bytes(64), 'if-phase-0', 4)
        for offset, value in ((0x10, 1), (0x11, 1), (0x12, 4), (0x13, 2), (0x15, 1)):
            data = self.memory('if-phase-0')
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'if-phase-0', 4)['authored_constraints_match'])

    def test_software_controls_require_a_serviced_request_and_handler_guard(self):
        for offset, value in ((0x10, 1), (0x11, 2), (0x12, 2), (0x14, 1), (0x15, 0)):
            data = self.memory('software-phase-0')
            data[offset] = value
            self.assertFalse(probe.analyze(data, 'software-phase-0', 4)['authored_constraints_match'])
        # IRQ controls cannot fall through the foreground to publish success.
        rom = probe.probe_rom('software-phase-0')
        self.assertNotIn(bytes.fromhex('c3 00 14'), rom[0x1000:0x1400])
        self.assertIn(bytes.fromhex('c3 00 14'), rom[0x300:0x380])

    def test_one_phase_if_difference_is_not_masked(self):
        a = self.memory('if-phase-7', True)
        b = bytearray(a)
        b[0x10] = 2
        x = probe.analyze(a, 'if-phase-7', 4)
        y = probe.analyze(b, 'if-phase-7', 4)
        self.assertTrue(x['authored_constraints_match'] and y['authored_constraints_match'])
        self.assertNotEqual(x['event_sha256'], y['event_sha256'])

    def report(self):
        rows = []
        for scenario in probe.SCENARIOS:
            for speed in (False, True):
                captures = {core: dict(probe.analyze(self.memory(scenario, speed), scenario, 4),
                                       repeatable=True) for core in ('gbb', 'sameboy')}
                rows.append(dict(scenario=scenario, double_speed=speed, frame=4, match=True, captures=captures))
        return dict(qualification=False, rows=rows)

    def test_complete_repeated_matrix_and_speed_witness(self):
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(probe.isolated, 'run_probe', return_value=self.report()):
            report = probe.run_probe(Path('/unused'), Path('/unused'), Path(tmp))
        self.assertTrue(probe.passes(report))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1])))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1] + [report['rows'][0]])))
        for field in ('repeatable', 'authored_constraints_match'):
            r = self.report()
            r['rows'][0]['captures']['gbb'][field] = False
            self.assertFalse(probe.passes(r))
        r = self.report()
        r['rows'][0]['captures']['gbb']['key1_speed'] = 128
        with tempfile.TemporaryDirectory() as tmp, patch.object(probe.isolated, 'run_probe', return_value=r):
            self.assertFalse(probe.passes(probe.run_probe(Path('/unused'), Path('/unused'), Path(tmp))))

    def test_cli_retains_reference_mismatches(self):
        report = self.report()
        report['rows'][0]['match'] = False
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused', '--sameboy-capture', '/unused',
                                           '--output', '/unused']), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


if __name__ == '__main__':
    unittest.main()
