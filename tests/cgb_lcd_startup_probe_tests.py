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
import cgb_lcd_startup_probe as probe


class LcdStartupContracts(unittest.TestCase):
    def test_owned_roms_and_headers(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 24)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x143], 0xc0)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
            self.assertEqual(rom[0x1100], 0xf3)  # Disable IRQs before freezing the log.
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    @staticmethod
    def memory(scenario, speed=False):
        source, observation, _ = probe.split(scenario)
        data = bytearray(8192)
        data[:2] = bytes((2, 1))
        data[0x3f] = 0xa5
        for i in range(2):
            ly = 144 if source == 'vblank' else i + 1
            mode = 1 if source == 'vblank' else 2
            request = (1 if source == 'vblank' else 2) if observation == 'poll' else 0
            data[0x10 + 7 * i:0x17 + 7 * i] = bytes((0, 10 + i, ly, mode, request, i, 128 if speed else 0))
        return data

    def test_request_poll_and_acknowledged_irq_are_distinct(self):
        for scenario in probe.SCENARIOS:
            for speed in (False, True):
                data = self.memory(scenario, speed)
                self.assertTrue(probe.analyze(data, scenario, 4)['authored_constraints_match'])
                request = 1 if scenario.startswith('vblank') else 2
                data[0x14] ^= request
                self.assertFalse(probe.analyze(data, scenario, 4)['authored_constraints_match'])

    def test_incomplete_and_malformed_samples_fail_closed(self):
        for index in (0, 1, 0x3f):
            data = self.memory('stat-irq-pad0')
            data[index] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'stat-irq-pad0', 4)
        with self.assertRaises(ValueError):
            probe.analyze(bytes(64), 'stat-irq-pad0', 4)
        for index, value in ((0x12, 154), (0x13, 4), (0x14, 4), (0x16, 1), (0x1d, 128)):
            data = self.memory('stat-irq-pad0')
            data[index] = value
            self.assertFalse(probe.analyze(data, 'stat-irq-pad0', 4)['authored_constraints_match'])

    def test_counter_difference_is_preserved(self):
        a = self.memory('stat-irq-pad0')
        b = bytearray(a)
        b[0x11] += 1
        x = probe.analyze(a, 'stat-irq-pad0', 4)
        y = probe.analyze(b, 'stat-irq-pad0', 4)
        self.assertTrue(x['authored_constraints_match'] and y['authored_constraints_match'])
        self.assertNotEqual(x['event_sha256'], y['event_sha256'])

    def report(self):
        rows = []
        for speed in (False, True):
            for scenario in probe.SCENARIOS:
                for frame in probe.CHECKPOINTS:
                    capture = dict(probe.analyze(self.memory(scenario, speed), scenario, frame), repeatable=True)
                    rows.append(dict(scenario=scenario, double_speed=speed, frame=frame, match=True,
                                     captures={c: dict(capture) for c in ('gbb', 'sameboy')}))
        return dict(qualification=False, rows=rows)

    def test_grid_repeatability_speed_and_frozen_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(probe.isolated, 'run_probe', return_value=self.report()):
                r = probe.run_probe(Path('/unused'), Path('/unused'), Path(tmp))
            self.assertTrue(probe.passes(r))
            self.assertFalse(probe.passes(dict(r, rows=r['rows'][:-1])))
            self.assertFalse(probe.passes(dict(r, rows=r['rows'][:-1] + [r['rows'][0]])))
        for field in ('repeatable', 'authored_constraints_match'):
            r = self.report()
            for row in r['rows']:
                row['frozen_log'] = True
            r['rows'][0]['captures']['gbb'][field] = False
            self.assertFalse(probe.passes(r))
        for mutation in ('speed', 'unfrozen'):
            r = self.report()
            if mutation == 'speed':
                r['rows'][0]['captures']['gbb']['events'][0][6] = 128
            else:
                r['rows'][0]['captures']['gbb']['event_sha256'] = 'changed'
            with tempfile.TemporaryDirectory() as tmp, patch.object(probe.isolated, 'run_probe', return_value=r):
                result = probe.run_probe(Path('/unused'), Path('/unused'), Path(tmp))
            self.assertFalse(probe.passes(result))

    def test_cli_does_not_turn_a_mismatch_into_validation(self):
        r = self.report()
        for row in r['rows']:
            row['frozen_log'] = True
        r['rows'][0]['match'] = False
        with patch.object(probe, 'run_probe', return_value=r), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused', '--sameboy-capture', '/unused',
                                           '--output', '/unused']), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


if __name__ == '__main__':
    unittest.main()
