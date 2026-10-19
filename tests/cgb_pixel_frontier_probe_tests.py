# SPDX-License-Identifier: GPL-3.0-or-later
from collections import Counter
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
import cgb_pixel_frontier_probe as probe

ADAPTER = Path(sys.argv.pop(1)).resolve()


class PixelFrontierContracts(unittest.TestCase):
    def test_authored_roms_are_distinct_with_valid_headers(self):
        cases = probe.scenarios()
        self.assertEqual(len(cases), 34)
        roms = [probe.probe_rom(case) for case in cases]
        self.assertEqual(len(set(roms)), 34)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)

    def test_frontier_handles_empty_full_and_rejects_nonmonotonic_rows(self):
        new, old = [8, 0, 0], [255, 255, 255]
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'row.ppm'
            for count in (0, 1, 159, 160):
                row = bytes(new) * count + bytes(old) * (160 - count)
                image.write_bytes(probe.HEADER + row * 144)
                self.assertEqual(probe.frontier(image, 16, new, old)['frontier'], count)
            image.write_bytes(probe.HEADER + (bytes(old) + bytes(new) * 159) * 144)
            with self.assertRaises(ValueError):
                probe.frontier(image, 0, new, old)
            with self.assertRaises(ValueError):
                probe.frontier(image, 144, new, old)
            with self.assertRaises(ValueError):
                probe.frontier(image, 0, old, old)
            image.write_bytes(b'P6\n160 144\n255\n')
            with self.assertRaises(ValueError):
                probe.frontier(image, 0, new, old)

    def test_runtime_initial_palette_controls_preserve_frontiers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for speed in (False, True):
                for scx in (0, 7):
                    takes = []
                    cases = [c for c in probe.scenarios() if c['first_frame'] and
                             c['speed'] == speed and c['scx'] == scx]
                    for case in cases:
                        rom = root / (case['name'] + '.gbc')
                        rom.write_bytes(probe.probe_rom(case))
                        prefix = root / case['name']
                        targets = [case['center'] + offset for offset in probe.OFFSETS]
                        result = subprocess.run([str(ADAPTER), str(rom), 'owned',
                                                 ','.join(map(str, targets)), str(prefix)],
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        metadata = probe.elapsed.validate_metadata(
                            [json.loads(line) for line in result.stdout.splitlines()], targets, 'gbb')
                        frontiers = []
                        for target, row in zip(targets, metadata[1:]):
                            self.assertEqual(row['completed_frames'], 0)
                            path = Path(str(prefix) + f'-{target}.ppm')
                            frontiers.append(probe.frontier(path, row['ly'], probe.rgb(case['red']),
                                                            [255, 255, 255])['frontier'])
                        self.assertEqual(frontiers, sorted(frontiers))
                        takes.append(frontiers)
                    self.assertEqual(takes[0], takes[1])

    def test_recorded_pairing_provenance_and_palette_independence(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-pixel-frontier.json').read_text())
        self.assertFalse(evidence['qualification'])
        for path, digest in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest)
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.anchor.owned_boot()).hexdigest())
        cases = {c['name']: c for c in probe.scenarios()}
        rows = evidence['rows']
        self.assertEqual(len(rows), 238)
        self.assertEqual({(r['scenario'], r['target_8mhz']) for r in rows},
                         {(name, c['center'] + offset) for name, c in cases.items() for offset in probe.OFFSETS})
        for row in rows:
            case = cases[row['scenario']]
            self.assertEqual(row['rom_sha256'], hashlib.sha256(probe.probe_rom(case)).hexdigest())
            self.assertTrue(row['repeatable'])
            a, b = (row['captures'][c] for c in ('gbb', 'sameboy'))
            for key in ('clocks_8mhz', 'ly', 'completed_frames'):
                self.assertEqual(a['metadata'][key], b['metadata'][key])
            self.assertEqual(a['metadata']['clocks_8mhz'], row['target_8mhz'])
            self.assertEqual(row['frontier_delta'], b['frontier'] - a['frontier'])
            self.assertEqual(row['match'], a['row_sha256'] == b['row_sha256'])
            self.assertEqual(row['match'], row['frontier_delta'] == 0)
            for capture in (a, b):
                self.assertTrue(0 <= capture['frontier'] <= 160)
                if case['first_frame']:
                    self.assertEqual(capture['metadata']['completed_frames'], 0)
                    self.assertEqual(capture['current_rgb'], probe.rgb(case['red']))
            if row['scenario'].endswith('-red1'):
                other = next(r for r in rows if r['scenario'] == row['scenario'][:-1] + '2'
                             and r['target_8mhz'] == row['target_8mhz'])
                for core in ('gbb', 'sameboy'):
                    self.assertEqual(row['captures'][core]['frontier'], other['captures'][core]['frontier'])
        self.assertEqual(Counter(r['frontier_delta'] for r in rows), {0: 44, 1: 4, 2: 190})
        rejected = evidence['rejected_candidate']
        self.assertFalse(rejected['accepted'])
        self.assertEqual(rejected['exact_matches'], 44)
        self.assertEqual(rejected['remaining_one_pixel_mismatches'], 194)
        self.assertEqual(rejected['contracts'][0]['exit_code'], 1)

    def test_mismatched_frontier_is_not_a_successful_validation(self):
        with patch.object(sys, 'argv', ['probe', '--gbb-capture', str(ADAPTER),
                                       '--sameboy-capture', str(ADAPTER), '--output', '/tmp/unused']), \
             patch.object(probe, 'run_probe', return_value={'rows': [{'match': False}]}), \
             patch('builtins.print'):
            self.assertEqual(probe.main(), 1)


if __name__ == '__main__':
    unittest.main()
