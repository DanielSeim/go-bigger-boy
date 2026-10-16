#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hardware witness headers, bounded failure, raw display and result protocol."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_cgb_irq_div_hardware as hardware


class Contracts(unittest.TestCase):
    def test_hardware_header_and_measurement_preserved(self):
        for source in ('timer', *hardware.probe.SOFTWARE_OFFSETS):
            for speed in (False, True):
                case = f'{source}-start-44-read-3'
                rom = hardware.hardware_rom(case, speed)
                base = hardware.probe.probe_rom(case, speed)
                self.assertEqual(len(rom), 32768)
                self.assertEqual(rom[0x104:0x134], hardware.LOGO)
                self.assertEqual(rom[0x143], 0xc0)
                self.assertEqual(rom[0x50:0x60], base[0x50:0x60])
                end = base.index(bytes(16) + bytes.fromhex('18 fe'), 0x150, 0x300)
                self.assertEqual(rom[0x150:end + 16], base[0x150:end + 16])
                self.assertEqual(rom[end + 16:end + 19], bytes.fromhex('c3 00 09'))
                checksum = 0
                for value in rom[0x134:0x14d]:
                    checksum = (checksum - value - 1) & 255
                self.assertEqual(rom[0x14d], checksum)
                self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                                 sum(rom[:0x14e] + rom[0x150:]) & 65535)

    def test_font_and_manifest_are_deterministic(self):
        self.assertEqual(len(hardware.GLYPHS), 39)
        self.assertTrue(all(len(glyph) == 15 for glyph in hardware.GLYPHS.values()))
        self.assertEqual(hardware.tiles()[:16], bytes(16))
        with tempfile.TemporaryDirectory() as tmp:
            a = hardware.build(Path(tmp) / 'a')
            b = hardware.build(Path(tmp) / 'b')
            self.assertEqual(a, b)
            self.assertFalse(a['qualification'])
            self.assertEqual(len(a['rows']), 8)
            with self.assertRaises(FileExistsError):
                hardware.build(Path(tmp) / 'a')


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set paired native capture tools')
class NativeContracts(unittest.TestCase):
    def test_raw_witnesses_display_and_protocol(self):
        runner = Path(os.environ['GBB_TEST_RUNNER']).resolve()
        adapter = Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cases = tuple(f'{s}-start-44-read-3' for s in ('timer', *hardware.probe.SOFTWARE_OFFSETS))
            def analyze(memory, scenario, frame):
                result = hardware.probe.analyze(memory, scenario, frame)
                self.assertEqual(memory[3], 0)
                # Persistent screen tile map is checked from GBB VRAM below.
                return result
            report = hardware.probe.isolated.run_probe(runner, adapter, root / 'captures',
                scenarios=cases, checkpoints=(4,), rom_builder=hardware.hardware_rom,
                analyzer=analyze, comparison_key='event_sha256', comparison_name='hardware_witness')
            self.assertEqual(len(report['rows']), 8)
            baseline = hardware.probe.isolated.run_probe(runner, adapter, root / 'baseline',
                scenarios=cases, checkpoints=(4,), rom_builder=hardware.probe.probe_rom,
                analyzer=hardware.probe.analyze, comparison_key='event_sha256',
                comparison_name='original_witness')
            for current, original in zip(report['rows'], baseline['rows']):
                self.assertEqual(current['scenario'], original['scenario'])
                self.assertEqual(current['double_speed'], original['double_speed'])
                for emulator in ('gbb', 'sameboy'):
                    self.assertEqual(current['captures'][emulator]['observations'],
                                     original['captures'][emulator]['observations'])

            for row in report['rows']:
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['record_structure_match'])
            boot = root / 'boot.bin'
            boot.write_bytes(hardware.probe.isolated.anchor.owned_boot())
            for speed in (False, True):
                for case in cases:
                    name = f'{case}-{"double" if speed else "normal"}'
                    rom = root / 'captures' / (name + '.gbc')
                    for command in ([str(runner), str(rom), '--model', 'cgb-e', '--dmg-boot',
                                     '--protocol', 'mooneye', '--max-cycles', '20000000'],
                                    [str(adapter), '--mooneye', str(rom), str(boot), 'cgb-e', '40000000']):
                        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                    # Compare rendered DIV digits with captured memory, preserving
                    # the original machine-readable observations.
                    prefix = root / 'captures' / (name + '-4-gbb-1-4')
                    vram = Path(str(prefix) + '.vram').read_bytes()
                    memory = Path(str(prefix) + '.state').read_bytes()
                    expected = bytes(hardware.TILES[c] for c in f'DIV {memory[0x100]:02X}')
                    self.assertEqual(vram[0x1800 + 6 * 32:0x1800 + 6 * 32 + len(expected)], expected)
                    done = bytes(hardware.TILES[c] for c in 'DONE')
                    self.assertEqual(vram[0x1800 + 11 * 32:0x1800 + 11 * 32 + 4], done)
            # Losing the interrupt must report failure instead of waiting forever.
            failed = root / 'missing-irq.gbc'
            failed.write_bytes(hardware.hardware_rom(cases[0], missing_irq=True))
            for command in ([str(runner), str(failed), '--model', 'cgb-e', '--dmg-boot',
                             '--protocol', 'mooneye', '--max-cycles', '20000000'],
                            [str(adapter), '--mooneye', str(failed), str(boot), 'cgb-e', '40000000']):
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 1, result.stderr)
            prefix = root / 'failed'
            result = subprocess.run([str(runner), str(failed), '--model', 'cgb-e', '--dmg-boot',
                '--frame-series', '4', '4', str(prefix), '--frame-state-series',
                '--max-cycles', '20000000'], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            vram = (root / 'failed-4.vram').read_bytes()
            memory = (root / 'failed-4.state').read_bytes()
            self.assertEqual(memory[3], 1)
            error = bytes(hardware.TILES[c] for c in 'ERROR')
            self.assertEqual(vram[0x1800 + 11 * 32:0x1800 + 11 * 32 + 5], error)



if __name__ == '__main__':
    unittest.main()
