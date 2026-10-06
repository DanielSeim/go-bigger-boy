#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Address-only observation through real SPC execution and trace CLI checks."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_sgb_prototype import build
RUNNER = TRACE = None


class ScoreTableReadContracts(unittest.TestCase):
    def test_real_spc_reads_and_write_order(self):
        result = subprocess.run([str(RUNNER)], check=True, capture_output=True, text=True)
        data = json.loads(result.stdout)
        sample = data['sample']
        # MOV abs,A performs a physical dummy read before its accepted write.
        self.assertEqual(sample['reads'], 5)
        self.assertEqual((sample['writes'], sample['base_writes']), (1, 1))
        self.assertFalse(sample['overflow'])
        self.assertFalse(sample['qualification'])
        self.assertEqual([e['address'] for e in sample['events']], [0x2b00, 0x2b01, 0x2b00, 0x2b02, 0x2b05])
        self.assertEqual([e['base_writes'] for e in sample['events']], [0, 0, 0, 1, 1])
        self.assertTrue(all(e['audible_packets'] == 7 for e in sample['events']))
        self.assertEqual(set(sample['events'][0]), {'master_clock', 'spc_half_clock', 'address', 'audible_packets', 'base_writes'})
        self.assertTrue(all(a['spc_half_clock'] < b['spc_half_clock'] for a, b in zip(sample['events'], sample['events'][1:])))
        self.assertEqual(data['empty']['events'], [])
        self.assertEqual(data['empty']['reads'], 0)
        self.assertFalse(data['empty']['overflow'])
        overflow = data['overflow']
        self.assertEqual(overflow['reads'], 300)
        self.assertEqual(len(overflow['events']), 256)
        self.assertTrue(overflow['overflow'])
        self.assertTrue(all(e['address'] == 0x2b00 for e in overflow['events']))

    def test_trace_cli_output_and_existing_file_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            program, game, boot, ipl = [base / name for name in ('program.rom', 'game.gb', 'boot.rom', 'ipl.rom')]
            program.write_bytes(build()[0])
            game.write_bytes(bytes(32768))
            boot.write_bytes(bytes(256))
            ipl.write_bytes(bytes(64))
            output = base / 'reads.json'
            command = [str(TRACE), str(program), str(ipl), '--sync-gb-sgb2', str(game), str(boot),
                       '--fractional-apu-sync', '--instruction-limit', '1', '--score-table-read-output', str(output)]
            # One SNES instruction ends at the documented bounded exit, with
            # a valid empty observation report. No private firmware is used.
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 4)
            self.assertEqual(json.loads(output.read_text())['events'], [])
            original = output.read_bytes()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            self.assertEqual(output.read_bytes(), original)
            output.unlink()
            no_sync = [arg for arg in command if arg != '--fractional-apu-sync']
            self.assertEqual(subprocess.run(no_sync, capture_output=True).returncode, 2)
            self.assertFalse(output.exists())
            core = command + ['--core-apu-engine']
            self.assertEqual(subprocess.run(core, capture_output=True).returncode, 2)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    args = parser.parse_args()
    RUNNER, TRACE = args.runner, args.trace
    unittest.main(argv=[sys.argv[0]])
