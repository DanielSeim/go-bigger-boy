#!/usr/bin/env python3
"""Exercise cycle-based CLI captures with independently authored LCD controls."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

RUNNER = str(Path(sys.argv.pop(1)).resolve())
FRAME_CYCLES = 70224


def cartridge(program, *, color=False):
    rom = bytearray(0x8000)
    rom[0x100:0x103] = bytes((0xC3, 0x50, 0x01))  # JP 0150
    rom[0x143] = 0x80 if color else 0
    rom[0x150:0x150 + len(program)] = program
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    return rom


LCD_OFF = bytes((0xF3, 0xAF, 0xE0, 0x40))  # DI; XOR A; LDH [LCDC],A
LOOP = bytes((0x18, 0xFE))


class CycleFrameCaptureTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='gbb-cycle-capture-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.rom = self.root / 'authored.gb'
        self.output = self.root / 'capture.ppm'
        self.rom.write_bytes(cartridge(LCD_OFF + LOOP))

    def run_rom(self, *args):
        return subprocess.run([RUNNER, str(self.rom), *map(str, args)],
                              capture_output=True, text=True, timeout=20)

    def capture(self, count=2, *args):
        return self.run_rom('--cycle-frame-capture', count,
                            '--frame-output', self.output, *args)

    def check_capture(self, result, count, width=160, height=144):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        match = re.search(r'target_t_cycles=(\d+), cycles=(\d+)', result.stdout)
        self.assertIsNotNone(match, result.stdout)
        target, actual = map(int, match.groups())
        self.assertEqual(target, count * FRAME_CYCLES)
        self.assertGreaterEqual(actual, target)
        self.assertLess(actual - target, 24)  # At most the final CPU instruction.
        header = f'P6\n{width} {height}\n255\n'.encode()
        image = self.output.read_bytes()
        self.assertTrue(image.startswith(header))
        self.assertEqual(len(image), len(header) + width * height * 3)
        return actual

    def test_disabled_lcd_captures_at_cpu_budget(self):
        trace = self.root / 'cpu.txt'
        actual = self.check_capture(self.capture(2, '--trace-cpu', trace), 2)
        last = trace.read_text().splitlines()[-1]
        match = re.search(r'cycle=(\d+) duration=(\d+)', last)
        start, duration = map(int, match.groups())
        self.assertLess(start, 2 * FRAME_CYCLES)
        self.assertEqual(start + duration, actual)
        # The existing PPU-frame mode still times out with LCDC disabled.
        old_output = self.root / 'old.ppm'
        old = self.run_rom('--frames', 2, '--frame-output', old_output,
                           '--max-cycles', 2 * FRAME_CYCLES)
        self.assertEqual(old.returncode, 2)
        self.assertIn('TIMEOUT', old.stderr)
        self.assertFalse(old_output.exists())

    def test_lcd_reenable_and_existing_frame_capture(self):
        # Fill tile 0 with color 3 while LCD is off, wait ~115k T-cycles,
        # then reenable it. The timed capture crosses the LCD-off interval.
        program = LCD_OFF + bytes((
            0x21, 0x00, 0x80, 0x06, 16, 0x3E, 0xFF,  # HL=8000; B=16; A=FF
            0x22, 0x05, 0x20, 0xFC,                  # Fill tile
            0x01, 0x00, 0x10,                        # BC=1000
            0x0B, 0x78, 0xB1, 0x20, 0xFB,            # DEC BC; A=B|C; JR NZ
            0x3E, 0xE4, 0xE0, 0x47,                  # BGP=E4
            0x3E, 0x91, 0xE0, 0x40)) + LOOP          # LCDC=91
        self.rom.write_bytes(cartridge(program))
        self.check_capture(self.capture(3), 3)
        timed = self.output.read_bytes()
        result = self.run_rom('--frames', 1, '--frame-output', self.output,
                              '--max-cycles', 4 * FRAME_CYCLES)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Captured frame 1', result.stdout)
        self.assertEqual(timed, self.output.read_bytes())

    def test_cgb_double_speed_uses_cpu_t_cycles(self):
        # KEY1 prepare; STOP switches speed; retain KEY1 read in A for the trace.
        program = LCD_OFF + bytes((0x3E, 1, 0xE0, 0x4D, 0x10, 0,
                                    0xF0, 0x4D)) + LOOP
        self.rom.write_bytes(cartridge(program, color=True))
        trace = self.root / 'cpu.txt'
        self.check_capture(self.capture(2, '--model', 'cgb-e', '--trace-cpu', trace), 2)
        last = trace.read_text().splitlines()[-1]
        af = int(re.search(r'af=([0-9a-f]+)', last).group(1), 16)
        self.assertTrue(af & 0x8000, 'KEY1 must confirm double speed')

    def test_sgb_border_and_stopped_cpu(self):
        self.check_capture(self.capture(2, '--model', 'sgb2', '--sgb-frame'), 2, 256, 224)
        self.rom.write_bytes(cartridge(LCD_OFF + bytes((0x10, 0)) + LOOP))
        self.check_capture(self.capture(2), 2)

    def test_cycle_limit_does_not_write_early_snapshot(self):
        result = self.capture(2, '--max-cycles', 2 * FRAME_CYCLES - 1)
        self.assertEqual(result.returncode, 2)
        self.assertIn('TIMEOUT', result.stderr)
        self.assertFalse(self.output.exists())
        # A target equal to the guard is still a successful capture.
        self.check_capture(self.capture(2, '--max-cycles', 2 * FRAME_CYCLES), 2)

    def test_invalid_and_conflicting_options(self):
        for count in ('0', '-1', '+1', '1x', '1.5', str((2**64 - 1) // FRAME_CYCLES + 1),
                      '18446744073709551616'):
            result = self.capture(count)
            self.assertEqual(result.returncode, 2, count)
            self.assertFalse(self.output.exists())
        for args in (('--frames', '1'), ('--frame-on-ld-bb',),
                     ('--frame-series', '1', '2', str(self.root / 'series')),
                     ('--cycle-frame-capture', '2'),
                     ('--replay-sgb-trace', str(self.root / 'unused.trace'))):
            result = self.capture(1, *args)
            self.assertEqual(result.returncode, 2, args)
            self.assertFalse(self.output.exists())
        result = self.run_rom('--cycle-frame-capture', '1')
        self.assertEqual(result.returncode, 2)
        self.assertIn('requires --frame-output', result.stderr)
        result = self.run_rom('--cycle-frame-capture')
        self.assertEqual(result.returncode, 2)

    def test_protocol_result_does_not_end_capture(self):
        # Publish a GBMicrotest pass before the cycle deadline.
        self.rom.write_bytes(cartridge(LCD_OFF + bytes((0x3E, 1, 0xE0, 0x82)) + LOOP))
        result = self.capture(2, '--protocol', 'gbmicrotest')
        self.check_capture(result, 2)
        self.assertNotIn('PASS (GBMicrotest)', result.stdout)


if __name__ == '__main__':
    unittest.main()
