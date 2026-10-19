# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_elapsed_anchor_probe as probe

ADAPTER = Path(sys.argv.pop(1)).resolve()


class ElapsedCaptureTests(unittest.TestCase):
    def run_capture(self, rom, mode, targets, prefix):
        return subprocess.run([str(ADAPTER), str(rom), mode, targets, str(prefix)],
                              capture_output=True, text=True, timeout=30)

    def test_capture_does_not_change_subsequent_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / 'owned.gbc'
            for speed in (False, True):
                rom.write_bytes(probe.anchor.probe_rom(speed))
                for mode in ('owned', 'post'):
                    captures = []
                    for take, targets in enumerate(('600000,1200000', '1200000')):
                        prefix = root / f'{speed}-{mode}-{take}'
                        result = self.run_capture(rom, mode, targets, prefix)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        metadata = probe.validate_metadata([json.loads(line) for line in result.stdout.splitlines()],
                                                           list(map(int, targets.split(','))), 'gbb')
                        captures.append((metadata[-1], Path(str(prefix) + '-1200000.ppm').read_bytes()))
                    self.assertEqual(captures[0], captures[1])
                    self.assertEqual(len(captures[0][1]), len(b'P6\n160 144\n255\n') + 160 * 144 * 3)

    def test_lcd_off_clock_and_exclusive_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / 'off.gbc'
            data = bytearray(probe.anchor.probe_rom())
            data[0x150:0x157] = bytes.fromhex('f3 af e0 40 c3 54 01')
            rom.write_bytes(data)
            prefix = root / 'capture'
            result = self.run_capture(rom, 'post', '1000,10000', prefix)
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata = [json.loads(line) for line in result.stdout.splitlines()]
            for row in metadata[1:]:
                self.assertEqual(row['completed_frames'], 0)
                self.assertEqual(row['lcdc'], 0)
                self.assertEqual(row['ly'], 0)
            image = Path(str(prefix) + '-10000.ppm')
            self.assertEqual(probe.anchor.read_capture(image)['unique_colors'], 1)
            original = image.read_bytes()
            self.assertEqual(self.run_capture(rom, 'post', '10000', prefix).returncode, 3)
            self.assertEqual(image.read_bytes(), original)

    def test_invalid_targets_modes_and_input_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / 'owned.gbc'
            rom.write_bytes(probe.anchor.probe_rom())
            for target in ('', '0', '-1', '+1', '1,', '2,1', '1,1', '1,,2', '200000001',
                           '99999999999999999999999999999', '1,2,3,4,5,6,7,8,9'):
                self.assertEqual(self.run_capture(rom, 'owned', target, root/'bad').returncode, 2)
            self.assertEqual(self.run_capture(rom, 'original', '100', root/'bad').returncode, 2)
            self.assertEqual(self.run_capture(root/'missing', 'owned', '100', root/'bad').returncode, 2)
            self.assertFalse(list(root.glob('bad*')))

    def test_reference_metadata_rejects_unsynchronized_or_wrong_anchor(self):
        row = dict(event='capture', schema='gbb-sameboy-elapsed-capture-v1', qualification=False,
                   target_8mhz=100, clocks_8mhz=100, ppu_synchronized=True,
                   completed_frames=0, pc=256, ly=0, lcdc=145,
                   anchor='elapsed_8mhz_after_boot_unmap_pc_0100')
        metadata = [dict(event='handoff', model='cgb-e', pc=0x100), row]
        probe.validate_metadata(metadata, [100], 'sameboy')
        for key, value in (('ppu_synchronized', False), ('qualification', True),
                           ('clocks_8mhz', 99), ('clocks_8mhz', 228), ('target_8mhz', 101),
                           ('ly', 154), ('completed_frames', -1), ('pc', True),
                           ('anchor', 'vblank'), ('schema', 'unknown')):
            changed = copy.deepcopy(metadata)
            changed[1][key] = value
            with self.assertRaises(ValueError):
                probe.validate_metadata(changed, [100], 'sameboy')

    def test_recorded_owned_evidence_keeps_disagreements_explicit(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-elapsed-anchor.json').read_text())
        self.assertFalse(evidence['qualification'])
        for path, digest in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest)
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.anchor.owned_boot()).hexdigest())
        rows = evidence['rows']
        self.assertEqual(len(rows), 20)
        self.assertEqual({(r['scenario'], r['target_8mhz']) for r in rows},
                         {(f'{s}-delay-{d}', t) for s in ('normal', 'double')
                          for d in (0, 4096) for t in probe.TARGETS})
        for row in rows:
            speed = row['scenario'].startswith('double')
            delay = int(row['scenario'].split('-')[-1])
            self.assertEqual(row['rom_sha256'], hashlib.sha256(probe.anchor.probe_rom(speed, delay)).hexdigest())
            self.assertTrue(row['repeatable'])
            captures = row['captures']
            self.assertEqual(row['match'], captures['gbb']['image'] == captures['sameboy']['image'])
            self.assertEqual(row['different_pixels'] == 0, row['match'])
            for core in captures:
                probe.validate_metadata([row['handoffs'][core], captures[core]['metadata']],
                                        [row['target_8mhz']], core)
            for key in ('completed_frames', 'ly'):
                self.assertEqual(captures['gbb']['metadata'][key], captures['sameboy']['metadata'][key])
        self.assertEqual(sum(r['match'] for r in rows), 14)
        self.assertEqual(sum(r['different_pixels'] == 2 for r in rows), 3)

    def test_research_mismatch_is_nonzero(self):
        with patch.object(sys, 'argv', ['probe', '--gbb-capture', str(ADAPTER),
                                       '--sameboy-capture', str(ADAPTER), '--output', '/tmp/unused']), \
             patch.object(probe, 'run_probe', return_value={'rows': [{'match': False}]}), \
             patch('builtins.print'):
            self.assertEqual(probe.main(), 1)


if __name__ == '__main__':
    unittest.main()
