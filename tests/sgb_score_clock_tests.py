#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free native score clock lifecycle and report checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_clock import build
from check_sgb_score_clock_reference import compare, native, observe

PROBE = None


class ClockTests(unittest.TestCase):
    def test_native_lifecycle(self):
        intervals, _ = native(PROBE)
        for slow, fast in zip(intervals[96], intervals[192]):
            self.assertTrue(0.47 <= fast/slow <= 0.53)

    def test_owned_image_reproducible(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '2e8f077e1038e1f53fe6e8acc0f5d82fe652676f6b4815248209007324a85b57')

    def test_probe_rejects_invalid_sizes(self):
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory)/'bad.bin'
            for size in (0, 513):
                program.write_bytes(bytes(size))
                result = subprocess.run([str(PROBE), str(program)], capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')

    def test_report_fails_closed(self):
        report = {'schema': 'gbb-spc-score-clock-v1', 'qualification': False, 'playback': False,
                  'unsupported_tempos_rejected': True, 'runs': []}
        for tempo in (96, 192):
            report['runs'].append({'tempo': tempo, 'tick_half_cycles':
                                  [i*(10800 if tempo == 96 else 5400) for i in range(1, 97)],
                                  'reset_equal': True, 'restore_equal': True, 'rollover': True,
                                  'pending_pulses_equal': True})
        observe(report)
        for malformed in (None, [], {}, {**report, 'runs': [None, None]}):
            with self.assertRaises(ValueError):
                observe(malformed)
        for key in ('qualification', 'playback'):
            altered = copy.deepcopy(report)
            altered[key] = True
            with self.assertRaises(ValueError):
                observe(altered)

        for key, value in (('restore_equal', False), ('tick_half_cycles', [1]*96)):
            altered = copy.deepcopy(report)
            altered['runs'][0][key] = value
            with self.assertRaises(ValueError):
                observe(altered)

    def test_reference_tolerance_fails_closed(self):
        references = []
        for model in ('sgb', 'sgb2'):
            for case, tempo, interval, gate in (
                    ('baseline', 96, 87000, 74000),
                    ('double-tempo', 192, 43500, 36000),
                    ('short-gate', 96, 87000, 46000)):
                references.append({'model': model, 'case': case, 'tempo': tempo,
                                   'onset_intervals_spc_cycles': [interval]*2,
                                   'gate_spc_cycles': [gate]*3})
        self.assertEqual(len(compare({96: [88060, 86020], 192: [43012, 43005]}, references)), 6)
        with self.assertRaises(ValueError):
            compare({96: [90000, 87000], 192: [43500]*2}, references)
        with self.assertRaises(ValueError):
            compare({96: [87000]*2, 192: [43500]*2}, references[:-1])

    def test_builder_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'clock.bin'
            output.write_bytes(b'keep')
            result = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1]/
                                    'scripts/build_sgb_score_clock.py'), '--output', str(output)],
                                    capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b'keep')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
