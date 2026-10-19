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
# Explicit FF DMA bus data: power-on WRAM is intentionally nonuniform.
FILL_DMA_SOURCE = bytes.fromhex('21 00 c0 06 a0 3e ff 22 05 20 fc')
CLEAR_BG_MAP = bytes.fromhex('21 00 98 01 00 04 36 00 23 0b 78 b1 20 f8')


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
        )) + CLEAR_BG_MAP + bytes((
            0x3E, 0xE4, 0xE0, 0x47,
            0x3E, 0x91, 0xE0, 0x40)) + LOOP
        self.rom.write_bytes(cartridge(program))
        result = self.capture(4)
        self.check_capture(result, 4)
        fields = dict(part.split('=') for part in result.stderr.split()[1:])
        self.assertEqual(fields['lcdc'], '91')
        self.assertEqual(fields['unique_colors'], '2')
        self.assertEqual(fields['blank_frame'], 'false')
        self.assertEqual(int(fields['completed_frames']), 3)
        timed = self.output.read_bytes()
        old = self.run_rom('--frames', 3, '--frame-output', self.output,
                           '--max-cycles', 5 * FRAME_CYCLES)
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

    def flow_lines(self, result, prefix):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = []
        for line in result.stderr.splitlines():
            if not line.startswith(prefix + ' '):
                continue
            self.assertRegex(line, r'^' + prefix + r' cycle=\d+ duration=\d+ '
                             r'pc=(?:[0-9a-f]{4}|none) opcode=(?:[0-9a-f]{2}|none) '
                             r'next_pc=[0-9a-f]{4} (?:sp=[0-9a-f]{4}|sp_before=[0-9a-f]{4} sp_after=[0-9a-f]{4}) af=[0-9a-f]{4} '
                             r'bc=[0-9a-f]{4} de=[0-9a-f]{4} hl=[0-9a-f]{4} normalized_8mhz=\d+'
                             r'(?: transition=(?:instruction|interrupt|initial))?$')
            rows.append(dict(part.split('=') for part in line.split()[1:]))
        return rows

    def test_flow_rst_first_hit_and_ring_transparency(self):
        program = LCD_OFF + bytes((0x31, 0x00, 0xD0, 0x01, 0x34, 0x12,
                                    0x11, 0x78, 0x56, 0x21, 0xBC, 0x9A,
                                    0xFF, 0x18, 0xFD))
        rom = cartridge(program);rom[0x38] = 0xC9  # RET from legitimate RST 38h.
        self.rom.write_bytes(rom)
        baseline_trace = self.root / 'baseline-cpu.txt'
        baseline = self.capture(2, '--trace-cpu', baseline_trace);image = self.output.read_bytes()
        trace = self.root / 'flow-reference.txt'
        combined = self.capture(2, '--trace-recent-cpu', 3, '--trace-first-pc', '0x0038',
                                '--trace-cpu', trace)
        self.assertEqual(combined.stdout, baseline.stdout)
        self.assertEqual(self.output.read_bytes(), image)
        self.assertEqual(trace.read_bytes(), baseline_trace.read_bytes())
        self.assertEqual(combined.stderr.splitlines()[-1], baseline.stderr.strip())
        first, = self.flow_lines(combined, 'CPU_FIRST_PC')
        self.assertEqual({k:first[k] for k in ('pc','opcode','next_pc','sp','af','bc','de','hl','transition')},
                         dict(pc='0160',opcode='ff',next_pc='0038',sp='cffe',af='0080',
                              bc='1234',de='5678',hl='9abc',transition='instruction'))
        recent = self.flow_lines(combined, 'CPU_RECENT')
        history = self.flow_lines(combined, 'CPU_FIRST_PC_HISTORY')
        self.assertEqual(len(history), 9)
        self.assertEqual((history[-1]['pc'], history[-1]['sp_before'], history[-1]['sp_after']),
                         ('0160', 'd000', 'cffe'))
        self.assertEqual(len(recent), 3)
        for row, line in zip(recent, trace.read_text().splitlines()[-3:]):
            expected = dict(part.split('=') for part in line.split())
            for key, value in row.items():
                if key != 'normalized_8mhz': self.assertEqual(value, expected[key])
            self.assertEqual(int(row['normalized_8mhz']),
                             2 * (int(row['cycle']) + int(row['duration'])))
        self.assertTrue(all(line.startswith('CPU_RECENT ') for line in combined.stderr.splitlines()[-4:-1]))
        for args, prefix in ((('--trace-recent-cpu', 1), 'CPU_RECENT'),
                             (('--trace-first-pc', '56'), 'CPU_FIRST_PC')):
            result = self.capture(2, *args)
            self.assertEqual(len(self.flow_lines(result, prefix)), 1)
            self.assertEqual(self.output.read_bytes(), image)
        cgb = self.capture(2, '--model', 'cgb-e', '--trace-first-pc', '0x38')
        first, = self.flow_lines(cgb, 'CPU_FIRST_PC')
        self.assertEqual((first['pc'], first['opcode'], first['next_pc']), ('0160', 'ff', '0038'))

    def test_flow_idle_steps_and_initial_or_unreached_pc(self):
        for idle in (bytes((0x76,)), bytes((0x10, 0))):
            self.rom.write_bytes(cartridge(LCD_OFF + idle + LOOP))
            result = self.capture(2, '--trace-recent-cpu', 4096)
            rows = self.flow_lines(result, 'CPU_RECENT')
            self.assertEqual([r['opcode'] for r in rows], ['c3','f3','af','e0',f'{idle[0]:02x}'])
            self.assertLess(len(result.stderr), 2048)
        initial = self.capture(2, '--trace-first-pc', '0x0100')
        row, = self.flow_lines(initial, 'CPU_FIRST_PC')
        self.assertEqual((row['pc'],row['opcode'],row['transition']), ('none','none','initial'))
        self.assertEqual(self.flow_lines(initial, 'CPU_FIRST_PC_HISTORY'), [])
        self.assertNotIn('CPU_STACK_DIAGNOSTICS', initial.stderr)
        for address in ('0xffff', '0'):
            absent = self.capture(2, '--trace-first-pc', address)
            self.assertEqual(self.flow_lines(absent, 'CPU_FIRST_PC'), [])
            self.assertEqual(self.flow_lines(absent, 'CPU_FIRST_PC_HISTORY'), [])
            self.assertNotIn('CPU_STACK_DIAGNOSTICS', absent.stderr)

    def test_first_pc_history_wrap_and_return_through_zero(self):
        # Return through an authored zero stack word, then execute RST 38h
        # at address 0000. More than 64 instructions exercise ring wrap.
        program = LCD_OFF + bytes((0x31,0,0xD0,0x01,0x34,0x12,
                                    0x11,0x78,0x56,0x21,0xBC,0x9A))
        program += bytes(80) + bytes((0xAF,0xEA,0xFE,0xCF,0xEA,0xFF,0xCF,
                                     0x31,0xFE,0xCF,0xC9))
        rom = cartridge(program, color=True)
        rom[0] = 0xFF;rom[0x38:0x3A] = LOOP
        self.rom.write_bytes(rom)
        baseline = self.capture(2, '--model', 'cgb-e')
        self.check_capture(baseline, 2);image = self.output.read_bytes()
        expected = []
        for index in range(22,80):
            expected.append((84+index*4, 0x160+index, 0, 0x161+index, 0xD000, 0xD000))
        expected += [(404,0x1B0,0xAF,0x1B1,0xD000,0xD000),
                     (408,0x1B1,0xEA,0x1B4,0xD000,0xD000),
                     (424,0x1B4,0xEA,0x1B7,0xD000,0xD000),
                     (440,0x1B7,0x31,0x1BA,0xD000,0xCFFE),
                     (452,0x1BA,0xC9,0,0xCFFE,0xD000),
                     (468,0,0xFF,0x38,0xD000,0xCFFE)]
        for extra in ((), ('--trace-recent-cpu',3), ('--trace-recent-cpu',128)):
            result = self.capture(2, '--model','cgb-e','--trace-first-pc','0x38',*extra)
            self.assertEqual(result.stdout, baseline.stdout)
            self.assertEqual(self.output.read_bytes(), image)
            self.assertEqual(result.stderr.splitlines()[-1], baseline.stderr.strip())
            first, = self.flow_lines(result, 'CPU_FIRST_PC')
            self.assertEqual(first['cycle'], '484')
            history = self.flow_lines(result, 'CPU_FIRST_PC_HISTORY')
            self.assertEqual(len(history), 64)
            stacks = [dict(item.split('=') for item in line.split()[1:])
                      for line in result.stderr.splitlines()
                      if line.startswith('CPU_STACK_DIAGNOSTICS ')]
            self.assertEqual(len(stacks), 64)
            for stack, instruction in zip(stacks, history):
                for key in ('cycle','pc','opcode','next_pc','sp_before','sp_after','af','bc','de','hl'):
                    self.assertEqual(stack[key], instruction[key])
                for key in ('stack_bytes','raw_stack_bytes','raw_stack_after'):
                    self.assertRegex(stack[key], r'^[0-9a-f]{16}$')
            actual = [(int(r['cycle']), *(int(r[k],16) for k in
                       ('pc','opcode','next_pc','sp_before','sp_after'))) for r in history]
            self.assertEqual(actual, expected)
            for r in history:
                self.assertEqual([r[k] for k in ('af','bc','de','hl')], ['0080','1234','5678','9abc'])
            lines = result.stderr.splitlines()
            begin = next(i for i,line in enumerate(lines) if line.startswith('CPU_FIRST_PC '))
            self.assertTrue(all(line.startswith('CPU_FIRST_PC_HISTORY ') for line in lines[begin+1:begin+65]))
        # Only explicitly requested captures exist; diagnostics create no files.
        self.assertEqual({p.name for p in self.root.iterdir()}, {'authored.gb','capture.ppm'})

    def test_stack_snapshot_distinguishes_dma_blocking_from_corruption(self):
        # No wait on purpose: RET while WRAM DMA is active must read FF.
        routine = bytes((0x3E,0xC0,0xE0,0x46,0,0,0xC9))
        program = LCD_OFF + FILL_DMA_SOURCE + bytes((0x31,0x02,0xD0,0x21,0x80,0xFF))
        for value in routine:
            program += bytes((0x3E,value,0x22))  # LD A,value; LD [HL+],A
        call_pc = 0x150 + len(program)
        program += bytes((0xCD,0x80,0xFF)) + LOOP
        rom = cartridge(program, color=True)
        rom[0x147] = 0x19  # MBC5; default ROM bank 1
        self.rom.write_bytes(rom)
        baseline = self.capture(2, '--model','cgb-e')
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        image = self.output.read_bytes()
        traced = self.capture(2, '--model','cgb-e','--trace-first-pc','0xffff')
        self.assertEqual(traced.returncode, 0, traced.stderr)
        self.assertEqual(traced.stdout, baseline.stdout)
        self.assertEqual(self.output.read_bytes(), image)
        self.assertEqual(traced.stderr.splitlines()[-1], baseline.stderr.strip())
        lines = [line for line in traced.stderr.splitlines()
                 if line.startswith('CPU_STACK_DIAGNOSTICS ')]
        self.assertGreater(len(lines), 0)
        self.assertLessEqual(len(lines), 64)
        fields = [dict(item.split('=') for item in line.split()[1:]) for line in lines]
        ret = next(r for r in fields if r['pc'] == 'ff86')
        for key, value in {'opcode':'c9', 'next_pc':'ffff', 'sp_before':'d000',
                           'sp_after':'d002', 'stack_bytes':'ffffffffffffffff',
                           'return_address':'ffff', 'dma_active':'true',
                           'dma_source':'c000', 'rom_bank':'001',
                           'double_speed':'false', 'snapshot':'before_step',
                           'raw_access':'bypasses_dma', 'raw_after_sp':'original'}.items():
            self.assertEqual(ret[key], value, ret)
        word = call_pc + 3
        expected = bytes((word & 255, word >> 8)).hex()
        self.assertTrue(ret['raw_stack_bytes'].startswith(expected))
        self.assertEqual(ret['raw_stack_bytes'], ret['raw_stack_after'])
        call = next(r for r in fields if r['pc'] == f'{call_pc:04x}')
        self.assertEqual((call['opcode'],call['next_pc'],call['sp_before'],call['sp_after']),
                         ('cd','ff80','d002','d000'))
        # Wrapping eight-byte snapshots at FFFF remain bounded and read only.
        self.rom.write_bytes(cartridge(LCD_OFF + bytes((0x31,0xFF,0xFF,0xFF))))
        wrapped = self.capture(2, '--trace-first-pc','0x38')
        self.assertEqual(wrapped.returncode, 0, wrapped.stderr)
        entries = [dict(item.split('=') for item in line.split()[1:])
                   for line in wrapped.stderr.splitlines()
                   if line.startswith('CPU_STACK_DIAGNOSTICS ')]
        self.assertEqual(entries[-1]['sp_before'], 'ffff')
        self.assertEqual(len(entries[-1]['stack_bytes']), 16)
        self.assertEqual(entries[-1]['rom_bank'], 'unavailable')

    def test_stack_snapshot_labels_dma_boundary_before_ret(self):
        routine = bytes((0x3E,0xC0,0xE0,0x46,0x3E,40,0x3D,0x20,0xFD,0xC9))
        program = LCD_OFF + FILL_DMA_SOURCE + bytes((0x31,0x02,0xD0,0x21,0x80,0xFF))
        for value in routine:
            program += bytes((0x3E,value,0x22))
        return_pc = 0x150 + len(program) + 3
        program += bytes((0xCD,0x80,0xFF)) + LOOP
        self.rom.write_bytes(cartridge(program, color=True))
        result = self.capture(2, '--model','cgb-e','--trace-first-pc',hex(return_pc))
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [dict(item.split('=') for item in line.split()[1:])
                for line in result.stderr.splitlines()
                if line.startswith('CPU_STACK_DIAGNOSTICS ')]
        ret = rows[-1]
        self.assertEqual((ret['pc'],ret['opcode'],ret['next_pc']),
                         ('ff89','c9',f'{return_pc:04x}'))
        # Snapshot precedes the opcode fetch. DMA finishes during RET, before
        # either actual stack read: the snapshot is not a bus transaction log.
        self.assertEqual(ret['snapshot'], 'before_step')
        self.assertEqual(ret['dma_active'], 'true')
        self.assertEqual(ret['dma_bytes_copied'], '159')
        self.assertEqual(ret['return_address'], 'ffff')
        self.assertEqual(ret['raw_stack_bytes'], ret['raw_stack_after'])

    def test_first_pc_history_excludes_idle_and_interrupt_steps(self):
        # HALT until a timer IRQ. Its many idle steps must not evict history.
        program = LCD_OFF + bytes((0x3E,4,0xEA,0xFF,0xFF,0xAF,0xE0,0x0F,
                                    0xE0,0x05,0x3E,5,0xE0,0x07,0xFB,0,0x76)) + LOOP
        rom = cartridge(program);rom[0x50:0x52] = bytes((0xF3,0x76))
        self.rom.write_bytes(rom)
        result = self.capture(2, '--trace-first-pc','0x50')
        first, = self.flow_lines(result, 'CPU_FIRST_PC')
        self.assertEqual(first['transition'], 'interrupt')
        history = self.flow_lines(result, 'CPU_FIRST_PC_HISTORY')
        self.assertEqual([r['opcode'] for r in history],
                         ['c3','f3','af','e0','3e','ea','af','e0','e0','3e','e0','fb','00','76'])
        self.assertEqual(history[-1]['sp_before'], history[-1]['sp_after'])
        self.assertNotEqual(history[-1]['next_pc'], '0050')

    def test_native_interrupt_discards_prefetch_from_executed_history(self):
        # DIV reset is T0. TAC is enabled at T20, overflow occurs at T32,
        # and reload requests the timer interrupt at T36, during the NOP fetch.
        program = LCD_OFF + bytes.fromhex(
            '31 fe ff 3e 04 ea ff ff af e0 0f e0 07 '
            '3e ff e0 05 af e0 06 fb 00 af e0 04 3e 05 e0 07')
        bc_pc = 0x150 + len(program)
        program += bytes.fromhex('01 00 00')
        discarded_pc = 0x150 + len(program)
        program += bytes.fromhex('00 18 fd')
        rom = cartridge(program, color=True)
        handler = bytes.fromhex('f3 af e0 07 ea ff ff 76 18 fd')
        rom[0x50:0x50+len(handler)] = handler
        self.rom.write_bytes(rom)
        result = self.capture(2, '--model','cgb-e','--trace-first-pc','0x50')
        self.assertEqual(result.returncode, 0, result.stderr)
        first, = self.flow_lines(result, 'CPU_FIRST_PC')
        self.assertEqual(first['transition'], 'interrupt')
        history = self.flow_lines(result, 'CPU_FIRST_PC_HISTORY')
        self.assertEqual((history[-1]['pc'],history[-1]['opcode']), (f'{bc_pc:04x}','01'))
        self.assertNotIn(f'{discarded_pc:04x}', [r['pc'] for r in history])

    def test_flow_interrupt_and_dma_fetch(self):
        program = LCD_OFF + bytes((0x3E,1,0xEA,0xFF,0xFF,0xE0,0x0F,0xFB,0x00)) + LOOP
        rom = cartridge(program);rom[0x40:0x42] = bytes((0xF3,0x76))
        self.rom.write_bytes(rom)
        result = self.capture(2, '--trace-first-pc', '0x40', '--trace-recent-cpu', 32)
        first, = self.flow_lines(result, 'CPU_FIRST_PC')
        self.assertEqual(first['transition'], 'interrupt')
        self.assertEqual(first['opcode'], '00')  # Previous instruction, not a fictional interrupt opcode.
        rows = self.flow_lines(result, 'CPU_RECENT')
        self.assertFalse(any(r['next_pc']=='0040' for r in rows))
        # After DMA's startup delay, actual FF differs from the stored NOP.
        self.rom.write_bytes(cartridge(LCD_OFF + FILL_DMA_SOURCE + bytes((0x3E,0xC0,0xE0,0x46,0x00,0x00)) + LOOP))
        dma = self.capture(2, '--trace-first-pc', '0x38')
        first, = self.flow_lines(dma, 'CPU_FIRST_PC')
        self.assertEqual((first['pc'],first['opcode'],first['transition']), (f'{0x150 + len(LCD_OFF + FILL_DMA_SOURCE) + 5:04x}','ff','instruction'))

    def test_flow_option_validation_and_protocol_mode(self):
        for args in (('--trace-recent-cpu','0'),('--trace-recent-cpu','4097'),
                     ('--trace-recent-cpu','-1'),('--trace-recent-cpu','1x'),
                     ('--trace-recent-cpu','1','--trace-recent-cpu','2'),
                     ('--trace-first-pc','0x10000'),('--trace-first-pc','-1'),
                     ('--trace-first-pc','0x'),('--trace-first-pc','12x'),
                     ('--trace-first-pc','0','--trace-first-pc','1'),
                     ('--trace-first-pc',),('--trace-recent-cpu',)):
            result = self.capture(2, *args)
            self.assertEqual(result.returncode, 2, args)
            self.assertFalse(self.output.exists())
        self.rom.write_bytes(cartridge(LCD_OFF + bytes((0x3E,1,0xE0,0x82)) + LOOP))
        baseline = self.run_rom('--protocol','gbmicrotest')
        traced = self.run_rom('--protocol','gbmicrotest','--trace-first-pc','0x0150')
        self.assertEqual(traced.returncode, baseline.returncode)
        self.assertEqual(traced.stdout, baseline.stdout)
        self.assertEqual(len(self.flow_lines(traced, 'CPU_FIRST_PC')), 1)


if __name__ == '__main__':
    unittest.main()
