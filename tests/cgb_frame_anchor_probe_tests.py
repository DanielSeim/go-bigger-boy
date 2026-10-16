#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored CGB probe contracts; no external cartridge inputs required."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('probe', ROOT / 'scripts/cgb_frame_anchor_probe.py')
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)


class ProbeContracts(unittest.TestCase):
    def test_owned_roms_have_valid_headers_and_distinct_scenarios(self):
        roms = [PROBE.probe_rom(speed, delay) for speed in (False, True) for delay in (0, 4096)]
        self.assertEqual(len(set(roms)), 4)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
            self.assertEqual(rom[0x143], 0xc0)
            self.assertEqual(rom[0x147:0x14a], b'\x00\x00\x00')
        for delay in (-1, 65536):
            with self.assertRaises(ValueError):
                PROBE.probe_rom(delay=delay)

    def test_strict_capture_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'capture.ppm'
            for invalid in (b'', b'P6\n160 144\n255\n', b'P6\n1 1\n255\n\x08\x00\x00'):
                image.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    PROBE.read_capture(image)
            image.write_bytes(b'P6\n160 144\n255\n' + b'\x08\x00\x00' * (160 * 144))
            capture = PROBE.read_capture(image)
            self.assertEqual(capture['unique_colors'], 1)
            self.assertEqual(capture['solid_rgb'], [8, 0, 0])
            image.write_bytes(image.read_bytes()[:-3] + b'\x10\x00\x00')
            self.assertIsNone(PROBE.read_capture(image)['solid_rgb'])

    @staticmethod
    def fake_capture(command, **kwargs):
        sameboy = '--frames' not in command
        frame = int(command[3] if sameboy else command[command.index('--frames') + 1])
        output = Path(command[4] if sameboy else command[-1])
        color = bytes((frame << 3 | frame >> 2, 0, 0))
        if sameboy and frame == 1:
            color = bytes((255, 255, 255))
        output.write_bytes(b'P6\n160 144\n255\n' + color * (160 * 144))
        return subprocess.CompletedProcess(command, 0, json.dumps(
            dict(schema='gbb-sameboy-cgb-capture-v1', qualification=False,
                 completed_ppu_frames=frame, model='cgb-e', random_seed=0,
                 color_correction='disabled', capture_anchor='normal_vblank_including_boot')), '')

    def test_first_frame_mismatch_is_preserved_without_shifting(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'owned-fake-executable'
            executable.write_bytes(b'not invoked; subprocess mocked')
            with patch.object(PROBE.subprocess, 'run', side_effect=self.fake_capture) as calls:
                report = PROBE.run_probe(executable, executable, Path(tmp) / 'output')
            self.assertEqual(calls.call_count, 64)
            self.assertFalse(report['qualification'])
            self.assertEqual(len(report['rows']), 16)
            for row in report['rows']:
                self.assertEqual(row['pixel_match'], row['frame'] != 1)
                self.assertEqual(row['expected_marker_match'], row['frame'] != 1)
                self.assertTrue(all(c['repeatable'] for c in row['captures'].values()))
            with patch.object(PROBE, 'run_probe', return_value=report), \
                    patch.object(sys, 'argv', ['probe', '--runner', str(executable),
                                              '--sameboy-capture', str(executable),
                                              '--output', str(Path(tmp) / 'other')]), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(PROBE.main(), 1)

    def test_child_failure_does_not_write_success_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'output'
            with patch.object(PROBE.subprocess, 'run', return_value=
                              subprocess.CompletedProcess([], 1, '', 'budget exhausted')):
                with self.assertRaisesRegex(ValueError, 'capture failed'):
                    PROBE.run_probe(Path('/unused'), Path('/unused'), output)
            self.assertFalse((output / 'report.json').exists())

    def test_wrong_reference_metadata_fails_closed(self):
        def wrong_metadata(command, **kwargs):
            result = self.fake_capture(command, **kwargs)
            if '--frames' not in command:
                metadata = json.loads(result.stdout)
                metadata['completed_ppu_frames'] += 1
                result.stdout = json.dumps(metadata)
            return result

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'output'
            with patch.object(PROBE.subprocess, 'run', side_effect=wrong_metadata):
                with self.assertRaisesRegex(ValueError, 'metadata does not match'):
                    PROBE.run_probe(Path('/unused'), Path('/unused'), output)
            self.assertFalse((output / 'report.json').exists())

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileExistsError):
                PROBE.run_probe(Path('/unused'), Path('/unused'), Path(tmp))


@unittest.skipUnless(os.environ.get('GBB_SAMEBOY_CGB_CAPTURE') and os.environ.get('GBB_TEST_RUNNER'),
                     'set GBB_SAMEBOY_CGB_CAPTURE and GBB_TEST_RUNNER for paired native probes')
class NativeProbeContracts(unittest.TestCase):
    def test_pinned_reference_first_frame_and_later_frame_markers(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = PROBE.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'captures')
            for row in report['rows']:
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                self.assertEqual(row['captures']['gbb']['solid_rgb'], row['expected_rgb'])
                if row['frame'] == 1:
                    self.assertEqual(row['captures']['sameboy']['solid_rgb'], [255, 255, 255])
                    self.assertFalse(row['pixel_match'])
                else:
                    self.assertTrue(row['expected_marker_match'])
                    self.assertTrue(row['pixel_match'])


if __name__ == '__main__':
    unittest.main()
