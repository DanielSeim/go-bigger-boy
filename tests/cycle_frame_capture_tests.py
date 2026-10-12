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
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        diagnostic = re.fullmatch(
            r'CAPTURE_DIAGNOSTICS cycles=(\d+) pc=([0-9a-f]{4}) '
            r'lcdc=([0-9a-f]{2}) stat=([0-9a-f]{2}) ly=([0-9a-f]{2}) '
            r'dot=(\d+) mode=([0-3]) completed_frames=(\d+) '
            r'unique_colors=(\d+) blank_frame=(true|false)', lines[0])
        self.assertIsNotNone(diagnostic, result.stderr)
        fields = dict(part.split('=') for part in lines[0].split()[1:])
        self.assertEqual(int(fields['cycles']), actual)
        self.assertEqual(int(fields['stat'], 16) & 3, int(fields['mode']))
        self.assertLess(int(fields['dot']), 456)
        self.assertLessEqual(int(fields['ly'], 16), 153)
        pixels = image[len(header):]
        colors = {pixels[i:i + 3] for i in range(0, len(pixels), 3)}
        self.assertEqual(int(fields['unique_colors']), len(colors))
        self.assertEqual(fields['blank_frame'], str(len(colors) == 1).lower())
        return actual

    def test_disabled_lcd_captures_at_cpu_budget(self):
        trace = self.root / 'cpu.txt'
        result = self.capture(2, '--trace-cpu', trace)
        actual = self.check_capture(result, 2)
        fields = dict(part.split('=') for part in result.stderr.split()[1:])
        self.assertEqual(fields['pc'], '0154')
        for field in ('lcdc', 'ly', 'dot', 'mode', 'completed_frames'):
            self.assertEqual(int(fields[field], 16 if field in ('lcdc', 'ly') else 10), 0)
        self.assertEqual(fields['unique_colors'], '1')
        self.assertEqual(fields['blank_frame'], 'true')
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
        self.assertNotIn('CAPTURE_DIAGNOSTICS', old.stderr)
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
        self.assertNotIn('CAPTURE_DIAGNOSTICS', result.stderr)
        self.assertEqual(timed, self.output.read_bytes())

    def test_multicolor_capture_and_completed_frames(self):
        # Alternate color-3 and color-0 tile rows, then enable the LCD.
        program = LCD_OFF + bytes((
            0x21, 0x00, 0x80, 0x06, 8, 0x3E, 0xFF,
            0x22, 0x05, 0x20, 0xFC,
            0x06, 8, 0xAF, 0x22, 0x05, 0x20, 0xFC,
            0x3E, 0xE4, 0xE0, 0x47,
            0x3E, 0x91, 0xE0, 0x40)) + LOOP
        self.rom.write_bytes(cartridge(program))
        result = self.capture(3)
        self.check_capture(result, 3)
        fields = dict(part.split('=') for part in result.stderr.split()[1:])
        self.assertEqual(fields['lcdc'], '91')
        self.assertEqual(fields['unique_colors'], '2')
        self.assertEqual(fields['blank_frame'], 'false')
        self.assertEqual(int(fields['completed_frames']), 3)
        timed = self.output.read_bytes()
        old = self.run_rom('--frames', 3, '--frame-output', self.output,
                           '--max-cycles', 4 * FRAME_CYCLES)
        self.assertEqual(old.returncode, 0, old.stderr)
        self.assertNotIn('CAPTURE_DIAGNOSTICS', old.stderr)
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

    def test_cgb_disabled_lcd_after_600_intervals(self):
        self.rom.write_bytes(cartridge(LCD_OFF + LOOP, color=True))
        result = self.capture(600, '--model', 'cgb-e')
        self.check_capture(result, 600)
        fields = dict(part.split('=') for part in result.stderr.split()[1:])
        self.assertEqual(fields['cycles'], str(600 * FRAME_CYCLES))
        self.assertEqual(fields['lcdc'], '00')
        self.assertEqual(fields['completed_frames'], '0')
        self.assertEqual(fields['unique_colors'], '1')
        self.assertEqual(fields['blank_frame'], 'true')

    def test_sgb_border_and_stopped_cpu(self):
        self.check_capture(self.capture(2, '--model', 'sgb2', '--sgb-frame'), 2, 256, 224)
        self.rom.write_bytes(cartridge(LCD_OFF + bytes((0x10, 0)) + LOOP))
        self.check_capture(self.capture(2), 2)

    def test_cycle_limit_does_not_write_early_snapshot(self):
        result = self.capture(2, '--max-cycles', 2 * FRAME_CYCLES - 1)
        self.assertEqual(result.returncode, 2)
        self.assertIn('TIMEOUT', result.stderr)
        self.assertNotIn('CAPTURE_DIAGNOSTICS', result.stderr)
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
        protocol = self.run_rom('--protocol', 'gbmicrotest')
        self.assertEqual(protocol.returncode, 0, protocol.stderr)
        self.assertIn('PASS (GBMicrotest)', protocol.stdout)
        self.assertNotIn('CAPTURE_DIAGNOSTICS', protocol.stderr)


if __name__ == '__main__':
    unittest.main()
