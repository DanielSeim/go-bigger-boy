#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute owned vendor-format scores through GB LCD upload and real SPC/DSP."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_music_fixture import build, bank, payload
from build_sgb_score_atomic_fixture import build_cartridge

PROBE = None


class VendorMusicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='gbb-vendor-music-')
        cls.root = Path(cls.directory.name)
        cls.rom = cls.root / 'host.rom'
        cls.rom.write_bytes(firmware())

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_image(self, image, model='sgb2', mode='native'):
        game = self.root / 'game.gb'
        game.write_bytes(image)
        child = subprocess.run([str(PROBE), str(self.rom), str(game), model,
                                '30000000', mode, 'fixture'], capture_output=True,
                               text=True, timeout=90)
        self.assertEqual(child.returncode, 0, child.stderr)
        result = json.loads(child.stdout)
        self.assertEqual(result['schema'], 'gbb-sgb-vendor-music-v1')
        self.assertIs(result['qualification'], False)
        self.assertIs(result['restore_equal'], True)
        self.assertIs(result['reset_equal'], True)
        self.assertGreater(result['restores'], 0)
        self.assertEqual(result['host_status'], 0)
        return result

    def test_01_build_and_export(self):
        image = firmware()
        self.assertEqual(image, firmware())
        self.assertEqual(hashlib.sha256(image).hexdigest(), '0267b319ad503ea9ec57bb2ce9cbbbfb8ee0dd16ea872e63ec63e63e7a19ee28')
        self.assertEqual(len(image), 262144)
        self.assertEqual(int.from_bytes(image[0x7FDC:0x7FDE], 'little') ^
                         int.from_bytes(image[0x7FDE:0x7FE0], 'little'), 65535)
        self.assertEqual(sum(image) & 65535, int.from_bytes(image[0x7FDE:0x7FE0], 'little'))
        for script, expected in (('build_sgb_vendor_music.py', image),
                                 ('build_sgb_vendor_music_fixture.py', build())):
            output = self.root / (script+'.bin')
            command = [sys.executable, str(Path(__file__).resolve().parents[1]/'scripts'/script), '--output', str(output)]
            first = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 2)
            self.assertEqual(output.read_bytes(), expected)

    def test_02_real_dsp_and_scalar(self):
        for model in ('sgb', 'sgb2'):
            native = self.run_image(build(), model)
            scalar = self.run_image(build(), model, 'scalar')
            self.assertEqual(native['pcm_fnv64'], scalar['pcm_fnv64'])
            self.assertEqual(native['frames'], scalar['frames'])
            for result in (native, scalar, self.run_image(build(), model, 'combined')):
                self.assertEqual((result['starts'], result['completes'], result['notes']), (1, 1, 3))
                self.assertEqual((result['transfers'], result['adoptions']), (1, 2))
                self.assertEqual(result['firmware_state'], 1)
                self.assertEqual(result['rejected'], 0)
                self.assertGreater(result['nonzero'], 1000)
                self.assertLess(result['last_nonzero_clock'], 20000000)
                self.assertEqual(result['flg'], 0xE0)
            dry = self.run_image(build('dry'), model)
            self.assertNotEqual(native['pcm_fnv64'], dry['pcm_fnv64'])

    def test_03_separate_assets_and_lifecycle(self):
        for model in ('sgb', 'sgb2'):
            plain = self.run_image(build(), model)
            separate = self.run_image(build('split-assets'), model)
            self.assertEqual((separate['transfers'], separate['adoptions']), (2, 3))
            self.assertEqual((separate['starts'], separate['completes'], separate['notes']), (1, 1, 3))
            self.assertGreater(separate['nonzero'], 1000)
            for case, starts, completes, selected in (('switch', 3, 1, 3), ('stop', 1, 0, 1),
                                                      ('upload-active', 2, 1, 2)):
                result = self.run_image(build(case), model)
                self.assertEqual((result['starts'], result['completes'], result['selected']),
                                 (starts, completes, selected), (case, result))
                self.assertEqual(result['firmware_state'], 1)
                self.assertGreater(result['nonzero'], 100)
                self.assertEqual(result['flg'], 0xE0)
            muted = self.run_image(build('mute'), model)
            self.assertEqual((muted['notes'], muted['completes'], muted['nonzero']), (3, 1, 0))
            unmuted = self.run_image(build('unmute'), model)
            self.assertEqual(unmuted['completes'], 1)
            self.assertGreater(unmuted['nonzero'], 100)
            self.assertLess(unmuted['nonzero'], plain['nonzero'])

    def test_04_reject_before_any_note(self):
        for model in ('sgb', 'sgb2'):
            for fault in ('late-opcode', 'root-outside', 'root-before', 'channel',
                          'zero-tempo', 'instrument', 'filter', 'pan', 'truncated'):
                with self.subTest(model=model, fault=fault):
                    result = self.run_image(build(fault=fault), model)
                    self.assertEqual(result['firmware_state'], 255)
                    self.assertEqual(result['rejected'], 0xE2)
                    self.assertEqual((result['starts'], result['notes'], result['nonzero']), (0, 0, 0))
                    self.assertEqual(result['flg'], 0xE0)
            for case in ('code-write', 'asset-only-cold', 'bad-entry', 'asset-wrap', 'asset-overflow'):
                result = self.run_image(build(case), model)
                self.assertEqual(result['firmware_state'], 255)
                self.assertNotEqual(result['transfer_error'], 0)
                self.assertEqual((result['transfers'], result['starts'], result['nonzero']), (0, 0, 0))

    def test_06_owned_echo_profiles_and_full_send_mask(self):
        baseline = self.run_image(build())
        hashes = {baseline['pcm_fnv64']}
        for filter_id in (1, 2, 3):
            score = bytearray(bank())
            score[0x6D] = filter_id
            frame = payload(((0x2B00, bytes(score)),))
            image = build_cartridge((frame,), ((16,bytes((0x41,0,0,0,1)),None),))
            result = self.run_image(image)
            self.assertEqual((result['starts'], result['completes'], result['notes']), (1,1,3))
            self.assertGreater(result['nonzero'], 1000)
            hashes.add(result['pcm_fnv64'])
        self.assertEqual(len(hashes), 4)
        score = bytearray(bank())
        score[0x6F] = 255
        image = build_cartridge((payload(((0x2B00,bytes(score)),)),),
                                ((16,bytes((0x41,0,0,0,1)),None),))
        full_send = self.run_image(image)
        self.assertEqual(full_send['pcm_fnv64'], baseline['pcm_fnv64'])

    def test_07_private_report_validation_and_sanitization(self):
        from check_sgb_vendor_music_title import validate
        report = dict(schema='gbb-sgb-vendor-music-v1', qualification=False,
                      model='sgb2', mode='native', reset_equal=True, restore_equal=True,
                      firmware_state=1, transfer_error=0, transfers=2, adoptions=3,
                      sounds=3, starts=2, completes=1, notes=2, selected=1, rejected=0,
                      flg=224, host_status=0, external=0, clocks=1000000010,
                      frames=1489947, nonzero=60000, last_nonzero_clock=966000000,
                      restores=612, unread_restores=18, gb_frames=2668, pcm_fnv64=123)
        self.assertEqual(validate(report,'sgb2')['completes'],1)
        report['private_data'] = 'not forwarded'
        self.assertNotIn('private_data',validate(report,'sgb2'))
        for key in ('qualification','restore_equal','reset_equal','transfer_error','notes',
                    'completes','starts','rejected','nonzero','unread_restores','clocks','frames'):
            changed = report.copy()
            changed[key] = True if key == 'qualification' else (1 if report[key] == 0 else 0)
            with self.subTest(key=key), self.assertRaises(ValueError): validate(changed,'sgb2')
        with self.assertRaises(ValueError): validate(report,'sgb')
        changed = report.copy(); changed['notes'] = True
        with self.assertRaises(ValueError): validate(changed,'sgb2')

    def test_05_native_execution_bounds(self):
        # A phrase-list loop made of nine valid references must reject, rather
        # than playing its first pattern. A control-only track reaches read cap.
        for kind in ('patterns', 'reads', 'events', 'end-word'):
            score = bytearray(bank())
            if kind == 'patterns':
                score[0x10:0x24] = (0x2B40).to_bytes(2, 'little')*9 + bytes(2)
                score[0x40:0x50] = bytes(4)+(0x2B60).to_bytes(2, 'little')+bytes(10)
            elif kind == 'reads':
                score = bytearray(bank())+bytearray(3800)
                score[0x60:] = bytes((0xE1,10))*((len(score)-0x60)//2)
            elif kind == 'events':
                score = bytearray(bank())+bytearray(256)
                score[0x60:0x60+134] = bytes((1,127))+bytes((0x98,))*131+bytes(1)
            else:
                score[0:2] = (0x2BFF).to_bytes(2, 'little')
            frame = payload(((0x2B00, bytes(score)),))
            image = build_cartridge((frame,), ((16,bytes((0x41,0,0,0,1)),None),))
            result = self.run_image(image)
            self.assertEqual((result['firmware_state'], result['rejected'], result['nonzero']), (255, 0xE2, 0), (kind, result))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, rest = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0]]+rest)
