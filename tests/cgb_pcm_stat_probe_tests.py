#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline contracts and optional public-Core-API integration for authored probes."""
import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_pcm_stat_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_all_rom_headers_and_scenarios(self):
        roms = [probe.probe_rom(scenario, speed) for speed in (False, True)
                for scenario in probe.SCENARIOS]
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

    def test_scrolling_oracle(self):
        static = probe.expected_scroll('scroll-0')
        shifted = probe.expected_scroll('scroll-3')
        lines = probe.expected_scroll('scroll-ly')
        self.assertEqual(len(static), 160 * 144 * 3)
        self.assertEqual(static[:12], bytes((0, 0, 0, 66, 0, 0, 132, 0, 0, 255, 0, 0)))
        self.assertEqual(shifted[:3], bytes((255, 0, 0)))
        self.assertEqual(lines[:480], static[:480])
        self.assertEqual(lines[480:483], bytes((66, 0, 0)))
        self.assertEqual(lines[3 * 480:3 * 480 + 3], bytes((255, 0, 0)))
        with self.assertRaises(ValueError):
            probe.expected_scroll('pulse')

    @staticmethod
    def buffer():
        data = bytearray(8192)
        data[:2] = b'\x40\x01'
        data[0x200] = 0xa5
        return data

    def test_sampling_controls_and_invalid_nibbles(self):
        data = self.buffer()
        self.assertTrue(probe.samples(data, 'silence')['authored_constraints_match'])
        self.assertFalse(probe.samples(data, 'pulse')['authored_constraints_match'])
        data[0x100] = 15
        self.assertTrue(probe.samples(data, 'pulse')['authored_constraints_match'])
        data[0x100] = 16  # Would imply unexpected CH2 activity.
        self.assertFalse(probe.samples(data, 'pulse')['authored_constraints_match'])
        data[0x100] = 0
        data[0x101] = 15
        self.assertTrue(probe.samples(data, 'wave')['authored_constraints_match'])
        data[0x101] = 0xf0  # Would imply unexpected noise activity.
        self.assertFalse(probe.samples(data, 'wave')['authored_constraints_match'])

    def test_incomplete_samples_and_damaged_guard_rejected(self):
        for offset in (0, 1, 0x200):
            data = self.buffer()
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.samples(data, 'silence')
        with self.assertRaises(ValueError):
            probe.samples(bytes(512), 'silence')

    def test_agreement_alone_is_not_a_pass(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        row = dict(match=True, captures=dict(gbb=capture.copy(), sameboy=capture.copy()))
        self.assertTrue(probe.passes(dict(rows=[row])))
        row['captures']['sameboy']['authored_constraints_match'] = False
        self.assertFalse(probe.passes(dict(rows=[row])))
        row['captures']['sameboy']['authored_constraints_match'] = True
        row['captures']['gbb']['repeatable'] = False
        self.assertFalse(probe.passes(dict(rows=[row])))
        row['captures']['gbb']['repeatable'] = True
        row['match'] = False
        self.assertFalse(probe.passes(dict(rows=[row])))
        self.assertFalse(probe.passes(dict(rows=[])))

    def test_failed_child_produces_no_success_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'dummy'
            executable.write_bytes(b'not invoked')
            output = Path(tmp) / 'results'
            with patch.object(probe.subprocess, 'run', return_value=
                              subprocess.CompletedProcess([], 1, '', 'budget exhausted')):
                with self.assertRaisesRegex(ValueError, 'capture failed'):
                    probe.run_probe(executable, executable, output)
            self.assertFalse((output / 'report.json').exists())

    def test_default_and_extended_capture_budgets_reach_both_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / 'dummy'
            executable.write_bytes(b'not invoked')
            def capture(command, **kwargs):
                data = bytearray(8192)
                data[:2] = bytes((64, 1))
                data[0x200] = 0xa5
                if '--frame-series' in command:
                    prefix = command[command.index('--frame-series') + 3]
                    Path(prefix + '-2.state').write_bytes(data + bytes(160))
                    metadata = ''
                else:
                    Path(command[-1]).write_bytes(data)
                    metadata = json.dumps(dict(schema='gbb-sameboy-cgb-capture-v1', qualification=False,
                        completed_ppu_frames=2, model='cgb-e', random_seed=0, color_correction='disabled',
                        capture_anchor='normal_vblank_including_boot'))
                return subprocess.CompletedProcess(command, 0, metadata, '')
            for name, budgets, expected in (
                    ('defaults', {}, (20_000_000, 40_000_000)),
                    ('extended', dict(max_cpu_cycles=100_000_000, max_reference_clocks=200_000_000),
                     (100_000_000, 200_000_000))):
                with patch.object(probe.subprocess, 'run', side_effect=capture), \
                        patch.object(probe.anchor, 'read_capture', return_value=dict(rgb_sha256='same')):
                    report = probe.run_probe(executable, executable, root / name,
                                             scenarios=('silence',), checkpoints=(2,), **budgets)
                self.assertTrue(probe.passes(report))
                self.assertEqual(len(report['commands']), 8)
                for command in report['commands']:
                    if '--max-cycles' in command:
                        self.assertEqual(int(command[command.index('--max-cycles') + 1]), expected[0])
                    else:
                        self.assertEqual(int(command[-2]), expected[1])

    def test_invalid_capture_budgets_reject_before_output_creation(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'results'
            for key in ('max_cpu_cycles', 'max_reference_clocks'):
                for value in (0, -1, True, 1.5, '20'):
                    with self.assertRaisesRegex(ValueError, 'positive integer'):
                        probe.run_probe(Path('/unused'), Path('/unused'), output, **{key: value})
                    self.assertFalse(output.exists())


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for paired native probes')
class NativeProbeContracts(unittest.TestCase):
    def test_isolated_pcm_and_stat_scenarios_match(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 24)
            self.assertTrue(probe.passes(report))
            self.assertFalse(report['qualification'])
            for speed in (False, True):
                for scenario in ('silence', 'pulse', 'wave'):
                    rows = [r for r in report['rows'] if r['scenario'] == scenario
                            and r['double_speed'] == speed]
                    for emulator in ('gbb', 'sameboy'):
                        self.assertEqual(rows[0]['captures'][emulator]['samples'],
                                         rows[1]['captures'][emulator]['samples'])


if __name__ == '__main__':
    unittest.main()
