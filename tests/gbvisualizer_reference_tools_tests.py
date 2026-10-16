#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline build contracts; optional native tests use only an authored test ROM."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'reference_build', ROOT / 'scripts/build_gbvisualizer_reference.py')
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)
ADAPTER = os.environ.get('GBB_SAMEBOY_CGB_CAPTURE')


class BuildContracts(unittest.TestCase):
    def test_adaptation(self):
        source = 'rTest EQU $90\n  ld [c], a\n  ldh a, [rTest + 1] ; sample\n'
        adapted, counts = BUILD.adapt(source)
        self.assertEqual(adapted,
                         'DEF rTest EQU $90\n  ldh [c], a\n'
                         '  ldh a, [$FF00 | (rTest + 1)] ; sample\n')
        self.assertEqual(counts, dict(definitions=1, indirect_ldh=1, absolute_ldh=1))

    def test_unrelated_source_unchanged(self):
        source = '; label EQU 1\n  ld a, [hl]\n  ldh [c], a\n  db $90\n'
        adapted, counts = BUILD.adapt(source)
        self.assertEqual(adapted, source)
        self.assertFalse(any(counts.values()))

    def test_pin_is_complete(self):
        pin = json.loads(BUILD.PIN.read_text())
        self.assertRegex(pin['commit'], r'^[0-9a-f]{40}$')
        self.assertRegex(pin['sameboy_commit'], r'^[0-9a-f]{40}$')
        self.assertRegex(pin['rom_sha256'], r'^[0-9a-f]{64}$')
        self.assertEqual(pin['rom_size'], 32768)

    def test_wrong_revision_fails_before_tools_or_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'output'
            with patch.object(BUILD.subprocess, 'check_output', return_value='wrong') as call:
                with self.assertRaisesRegex(ValueError, 'clean pinned commit'):
                    BUILD.build(Path(tmp), output)
                self.assertEqual(call.call_count, 1)
            self.assertFalse(output.exists())

    def test_dirty_checkout_fails(self):
        pin = json.loads(BUILD.PIN.read_text())
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(BUILD.subprocess, 'check_output',
                              side_effect=[pin['commit'], ' M visualizer.asm']):
                with self.assertRaisesRegex(ValueError, 'clean pinned commit'):
                    BUILD.build(Path(tmp), Path(tmp) / 'output')

    def test_rom_hash_mismatch_fails_closed(self):
        pin = json.loads(BUILD.PIN.read_text())
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'source'
            source.mkdir()
            for name in ('visualizer.asm', 'gbhw.asm', 'music.gbm'):
                (source / name).write_bytes(b'; synthetic input\n')
            output = Path(tmp) / 'output'

            def run(command, **kwargs):
                if command[0] == 'rgbfix':
                    (Path(kwargs['cwd']) / 'visualizer.gbc').write_bytes(bytes(32768))

            versions = [tool + ' ' + pin['rgbds_version']
                        for tool in ('rgbasm', 'rgblink', 'rgbfix')]
            with patch.object(BUILD.subprocess, 'check_output',
                              side_effect=[pin['commit'], '', *versions]), \
                    patch.object(BUILD.subprocess, 'run', side_effect=run):
                with self.assertRaisesRegex(ValueError, 'pinned size/hash'):
                    BUILD.build(source, output)
            self.assertFalse(output.exists())


@unittest.skipUnless(ADAPTER, 'set GBB_SAMEBOY_CGB_CAPTURE for optional native Core API tests')
class NativeCaptureContracts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        # Independently authored cartridge: delay with LCD off, then enable and halt.
        self.rom = self.work / 'owned.gbc'
        self.write_rom(bytes.fromhex('f3 31 fe ff af e0 40 01 00 10 0b 78 b1 20 fb 3e 91 e0 40 76 18 fd'))
        self.boot = self.work / 'owned-boot.bin'
        header = (ROOT / 'firmware/gameboy/cgb_boot_image.hpp').read_text()
        self.boot.write_bytes(bytes(int(n, 16) for n in re.findall(r'0x([0-9a-fA-F]{2})', header)))
        self.assertEqual(self.boot.stat().st_size, 256)

    def write_rom(self, program):
        rom = bytearray(32768)
        rom[0x100:0x103] = bytes.fromhex('c3 50 01')
        rom[0x143] = 0xc0
        rom[0x150:0x150 + len(program)] = program
        checksum = 0
        for value in rom[0x134:0x14d]:
            checksum = (checksum - value - 1) & 255
        rom[0x14d] = checksum
        self.rom.write_bytes(rom)

    def capture(self, name, frames='3', model='cgb-e', budget='20000000'):
        return subprocess.run([ADAPTER, str(self.rom), str(self.boot), frames,
                               str(self.work / name), model, budget],
                              capture_output=True, text=True, timeout=20)

    def test_normal_frames_determinism_and_exclusive_output(self):
        first = self.capture('first.ppm')
        second = self.capture('second.ppm')
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        metadata = json.loads(first.stdout)
        self.assertEqual(metadata['completed_ppu_frames'], 3)
        self.assertGreater(metadata['artificial_callbacks'], 0)
        self.assertFalse(metadata['qualification'])
        self.assertEqual(first.stdout, second.stdout)
        data = (self.work / 'first.ppm').read_bytes()
        self.assertEqual(data[:15], b'P6\n160 144\n255\n')
        self.assertEqual(len(data), 15 + 160 * 144 * 3)
        self.assertEqual(data, (self.work / 'second.ppm').read_bytes())
        self.assertEqual(self.capture('first.ppm').returncode, 4)
        self.assertEqual(hashlib.sha256(data).digest(),
                         hashlib.sha256((self.work / 'first.ppm').read_bytes()).digest())

    def test_lcd_off_watchdog_writes_no_capture(self):
        self.write_rom(bytes.fromhex('f3 31 fe ff af e0 40 76 18 fd'))
        result = self.capture('off.ppm')
        self.assertEqual(result.returncode, 1)
        self.assertIn('clock budget exhausted', result.stderr)
        self.assertFalse((self.work / 'off.ppm').exists())
        self.assertEqual(result.stdout, '')

    def test_invalid_arguments_and_missing_rom(self):
        for count in ('0', '-1', '100001', '3oops'):
            self.assertEqual(self.capture('invalid.ppm', frames=count).returncode, 2)
        self.assertEqual(self.capture('invalid.ppm', model='dmg').returncode, 2)
        self.rom.unlink()
        self.assertEqual(self.capture('missing.ppm').returncode, 4)
        self.assertFalse((self.work / 'missing.ppm').exists())

    def test_optional_wram_snapshot_and_failure_preserve_existing_files(self):
        memory = self.work / 'memory.bin'
        image = self.work / 'memory.ppm'
        command = [ADAPTER, str(self.rom), str(self.boot), '3', str(image),
                   'cgb-e', '20000000', str(memory)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(memory.stat().st_size, 8192)
        self.assertEqual(json.loads(result.stdout)['completed_ppu_frames'], 3)
        baseline = self.capture('baseline.ppm')
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        self.assertEqual(result.stdout, baseline.stdout)
        self.assertEqual(image.read_bytes(), (self.work / 'baseline.ppm').read_bytes())
        saved = memory.read_bytes()
        image.unlink()
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 4)
        self.assertFalse(image.exists())
        self.assertEqual(memory.read_bytes(), saved)
        self.assertEqual(result.stdout, '')

    def test_mooneye_protocol_pass_fail_timeout_and_plain_breakpoint(self):
        def run(budget='20000000', model='cgb-e'):
            return subprocess.run([ADAPTER, '--mooneye', str(self.rom), str(self.boot), model, budget],
                                  capture_output=True, text=True, timeout=20)
        self.write_rom(bytes.fromhex('06 03 0e 05 16 08 1e 0d 26 15 2e 22 40 18 fe'))
        first, second = run(), run()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        data = json.loads(first.stdout)
        self.assertEqual(data['result'], 'pass')
        self.assertEqual((data['bc'], data['de'], data['hl']), (0x0305, 0x080d, 0x1522))
        self.write_rom(bytes.fromhex('06 42 0e 42 16 42 1e 42 26 42 2e 42 40 18 fe'))
        failed = run()
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(json.loads(failed.stdout)['result'], 'fail')
        self.write_rom(bytes.fromhex('40 18 fd'))  # Ordinary LD B,B is not a result.
        timed_out = run()
        self.assertEqual(timed_out.returncode, 3)
        self.assertEqual(json.loads(timed_out.stdout)['result'], 'timeout')
        self.assertEqual(run(model='dmg').returncode, 2)
        self.rom.unlink()
        self.assertEqual(run().returncode, 4)


if __name__ == '__main__':
    unittest.main()
