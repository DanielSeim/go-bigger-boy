#!/usr/bin/env python3
"""Authored cartridges only: write boundaries, bounded histories and dump ordering."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = str(Path(sys.argv.pop(1)).resolve())
LOGO = bytes.fromhex('ceed6666cc0d000b03730083000c000d0008111f8889000edccc6ee6ddddd999'
                     'bbbb67636e0eecccdddc999fbbb9333e')

def cartridge(program, *, mmm=True, menu_first=False):
    data = bytearray(32*16384 if mmm else 32768)
    offset = len(data)-32768 if mmm else 0
    data[offset+0x100:offset+0x103] = bytes.fromhex('c35001')
    data[offset+0x104:offset+0x134] = LOGO
    data[offset+0x147] = 0x0B if mmm else 0
    data[offset+0x148] = 4 if mmm else 0
    data[offset+0x150:offset+0x150+len(program)] = program
    data[offset+0x14D] = (-sum(data[offset+0x134:offset+0x14D])-25)&255
    if menu_first:
        data = data[-32768:]+data[:-32768]
    return data

def rows(text, prefix):
    return [dict(item.split('=',1) for item in line.split()[1:])
            for line in text.splitlines() if line.startswith(prefix+' ')]

class DiagnosticsTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='gbb-mmm-diag-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.rom = self.root/'authored.gb'
        self.ppm = self.root/'frame.ppm'
        self.trace = self.root/'mapper.log'

    def run_rom(self, program, *args, mmm=True, menu_first=False):
        self.rom.write_bytes(cartridge(program,mmm=mmm,menu_first=menu_first))
        result = subprocess.run([RUNNER,str(self.rom),'--cycle-frame-capture','1',
                                 '--frame-output',str(self.ppm),*map(str,args)],
                                capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr)
        return result

    def test_real_writes_not_state_diffs_and_limits(self):
        # Two equal writes still count, then an instruction with two bus writes.
        program = bytes.fromhex('f33e05ea0020ea002031341208002018fe')
        self.run_rom(program,'--trace-mmm01',self.trace)
        log=self.trace.read_text()
        records=rows(log,'MMM01_WRITE')
        self.assertEqual(len(records),4)
        self.assertEqual([r['address'] for r in records],['2000','2000','2000','2001'])
        self.assertEqual(records[0]['registers_after'],records[1]['registers_after'])
        self.assertEqual(records[2]['pc'],records[3]['pc'])
        self.assertEqual(int(records[3]['cycle'])-int(records[2]['cycle']),4)
        self.assertEqual(records[2]['bank0_after'],'30')
        self.run_rom(program,'--trace-mmm01',self.trace,'--trace-limit','2')
        self.assertEqual(len(rows(self.trace.read_text(),'MMM01_WRITE')),2)
        self.assertEqual(len(rows(self.trace.read_text(),'MMM01_TRUNCATED')),1)

    def test_lcd_write_boundary_and_same_capture(self):
        # LD [FF40],SP disables LCD four cycles before its FF41 write completes.
        program=bytes.fromhex('f33100000840ff18fe')
        normal=self.run_rom(program)
        image=self.ppm.read_bytes()
        diagnosed=self.run_rom(program,'--trace-first-lcd-off','--trace-mmm01',self.trace,
                               '--trace-first-pc','0x0157','--trace-recent-cpu','2')
        self.assertEqual(self.ppm.read_bytes(),image)
        self.assertEqual(rows(normal.stderr,'CAPTURE_DIAGNOSTICS'),
                         rows(diagnosed.stderr,'CAPTURE_DIAGNOSTICS'))
        lcd=rows(diagnosed.stderr,'LCD_OFF')
        self.assertEqual(len(lcd),1)
        self.assertEqual(int(lcd[0]['after_cycle'])-int(lcd[0]['cycle']),4)
        w=rows(diagnosed.stderr,'LCD_OFF_WRITE')[0]
        self.assertEqual(w['pc'],'0154')
        self.assertEqual(w['address'],'ff40')
        self.assertEqual(w['boundary'],'cpu_write')
        self.assertEqual(rows(diagnosed.stderr,'LCD_OFF_INSTRUCTION')[0]['next_pc'],'0157')
        self.assertTrue(rows(diagnosed.stderr,'CPU_FIRST_PC'))
        self.assertEqual(len(rows(diagnosed.stderr,'CPU_RECENT')),2)

    def test_nop_history_before_run_and_one_shot(self):
        prefix=bytes.fromhex('f3')+bytes([0x04])*70 # DI; 70 INC B
        program=prefix+bytes(300)+bytes.fromhex('c39701')
        result=self.run_rom(program,'--trace-nop-run','256')
        trigger=rows(result.stderr,'NOP_RUN')
        self.assertEqual(len(trigger),1)
        history=rows(result.stderr,'NOP_RUN_HISTORY')
        self.assertEqual(len(history),64)
        self.assertTrue(all(r['opcode']=='04' for r in history))
        self.assertEqual(rows(result.stderr,'NOP_RUN_START')[0]['matches_rom'],'true')
        self.assertEqual(rows(result.stderr,'NOP_RUN_START')[0]['bank0'],'30')
        self.assertEqual(int(history[-1]['cycle'])+4,int(trigger[0]['start_cycle']))

    def test_dma_blocked_mapper_write_is_absent(self):
        # Copy an authored routine into HRAM, then run it while main-bus DMA blocks ROM.
        # HRAM: LD A,C0; LDH [46],A; LD A,05; LD [2000],A; JR -2.
        hram=bytes.fromhex('3ec0e0463e05ea002018fe')
        program=bytearray([0xF3])
        for i,value in enumerate(hram):
            program+=bytes([0x3E,value,0xE0,0x80+i])
        program+=bytes.fromhex('c380ff')
        self.run_rom(program,'--trace-mmm01',self.trace)
        self.assertEqual(rows(self.trace.read_text(),'MMM01_WRITE'),[])

    def test_non_rom_nops_have_no_rom_offset(self):
        hram=bytes(8)+bytes.fromhex('18fe')
        program=bytearray([0xF3])
        for i,value in enumerate(hram):
            program+=bytes([0x3E,value,0xE0,0x80+i])
        program+=bytes.fromhex('c380ff')
        result=self.run_rom(program,'--trace-nop-run','4')
        start=rows(result.stderr,'NOP_RUN_START')[0]
        self.assertEqual(start['pc'],'ff80')
        self.assertEqual(start['in_rom'],'false')
        self.assertEqual(start['matches_rom'],'false')
        self.assertEqual(start['physical_offset'],'none')
        self.assertEqual(start['file_offset'],'none')

    def test_both_layouts_boot_menu(self):
        program=bytes.fromhex('f33e05ea002018fe')
        self.run_rom(program,'--trace-mmm01',self.trace)
        physical=self.ppm.read_bytes()
        log=self.trace.read_text()
        self.run_rom(program,'--trace-mmm01',self.trace,menu_first=True)
        self.assertEqual(self.ppm.read_bytes(),physical)
        normalized=self.trace.read_text()
        self.assertEqual(rows(normalized,'MMM01_STATE')[0]['layout'],'menu_first_normalized')
        normalized_fetch=rows(normalized,'MMM01_FETCH_FIRST')[0]
        physical_fetch=rows(log,'MMM01_FETCH_FIRST')[0]
        self.assertEqual(normalized_fetch.pop('file_offset'),'256')
        self.assertEqual(physical_fetch.pop('file_offset'),'491776')
        self.assertEqual(normalized_fetch,physical_fetch)
        self.assertEqual(rows(normalized,'MMM01_WRITE'),rows(log,'MMM01_WRITE'))

    def test_only_mmm_file_and_independent_triggers(self):
        program=bytes.fromhex('f3afE040')+bytes(8)+bytes.fromhex('18fe')
        result=self.run_rom(program,'--trace-mmm01',self.trace,mmm=False)
        self.assertFalse(self.trace.exists())
        self.assertFalse(rows(result.stderr,'LCD_OFF'))
        self.assertFalse(rows(result.stderr,'NOP_RUN'))
        result=self.run_rom(program,'--trace-nop-run','4',mmm=False)
        self.assertEqual(len(rows(result.stderr,'NOP_RUN')),1)
        self.assertFalse(rows(result.stderr,'LCD_OFF'))
        result=self.run_rom(program,'--trace-first-lcd-off',mmm=False)
        self.assertEqual(len(rows(result.stderr,'LCD_OFF')),1)
        self.assertFalse(rows(result.stderr,'NOP_RUN'))
        result=self.run_rom(program,'--trace-first-lcd-off','--trace-nop-run','4',
                            '--trace-mmm01',self.trace)
        self.assertTrue(rows(result.stderr,'LCD_OFF'))
        self.assertTrue(rows(result.stderr,'NOP_RUN'))

    def test_halt_idle_is_not_nop(self):
        result=self.run_rom(bytes.fromhex('f37600'),'--trace-nop-run','2')
        self.assertFalse(rows(result.stderr,'NOP_RUN'))

    def test_lcd_history_and_recent_writes_remain_bounded_after_cap(self):
        program=bytes.fromhex('f3')+bytes([0x04])*70
        for value in range(40):
            program+=bytes([0x3E,value,0xEA,0,0x20])
        program+=bytes.fromhex('afe04018fe')
        result=self.run_rom(program,'--trace-mmm01',self.trace,'--trace-limit','2',
                            '--trace-first-lcd-off')
        history=rows(result.stderr,'LCD_OFF_HISTORY')
        recent=rows(result.stderr,'LCD_OFF_MMM01_WRITE')
        self.assertEqual(len(history),64)
        self.assertEqual(history[-1]['opcode'],'e0')
        self.assertEqual(len(recent),32)
        self.assertEqual(recent[0]['value'],'08')
        self.assertEqual(recent[-1]['value'],'27')
        self.assertEqual(len(rows(self.trace.read_text(),'MMM01_WRITE')),2)

    def test_lower_window_mapping_changes_real_fetches_and_stays_locked(self):
        # The menu locks base bank 8, then execution continues at the same CPU
        # address in physical bank 8, without needing a CPU jump.
        prefix=bytes.fromhex('f33e08ea00203e30ea00603e40ea0000')
        game=bytes.fromhex('3e0fea00203e00ea00003effea002018fe')
        data=cartridge(prefix+bytes.fromhex('76'))
        continuation=0x150+len(prefix)
        data[8*16384+continuation:8*16384+continuation+len(game)]=game
        self.rom.write_bytes(data)
        result=subprocess.run([RUNNER,str(self.rom),'--cycle-frame-capture','1',
                               '--frame-output',str(self.ppm),'--trace-mmm01',str(self.trace),
                               '--trace-first-pc',hex(continuation+5)],capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr)
        writes=rows(self.trace.read_text(),'MMM01_WRITE')
        self.assertEqual(len(writes),6)
        self.assertEqual((writes[2]['bank0_before'],writes[2]['bank0_after']),('30','8'))
        self.assertEqual(writes[2]['locked_after'],'true')
        self.assertEqual((writes[3]['bank0_after'],writes[3]['bank1_after']),('8','15'))
        self.assertEqual(writes[4]['mapped_after'],'true')
        self.assertEqual(writes[5]['registers_after'],'400f0030')
        first=rows(result.stderr,'CPU_FIRST_PC')[0]
        self.assertEqual(first['pc'],f'{continuation+2:04x}')
        self.assertEqual(first['opcode'],'ea')

    def test_diagnostics_preserve_memory_snapshots(self):
        self.rom.write_bytes(cartridge(bytes.fromhex('f33e05ea002018fe')))
        snapshots=[]
        for enabled in (False,True):
            prefix=self.root/('diagnosed' if enabled else 'plain')
            args=[RUNNER,str(self.rom),'--frame-series','1','2',str(prefix),'--frame-state-series']
            if enabled:
                args+=['--trace-mmm01',str(self.trace),'--trace-nop-run','4','--trace-first-lcd-off']
            r=subprocess.run(args,capture_output=True,text=True,timeout=30)
            self.assertEqual(r.returncode,0,r.stderr)
            states=sorted(self.root.glob(prefix.name+'*.state'))
            self.assertEqual(len(states),2)
            snapshots.append([p.read_bytes() for p in states])
        self.assertEqual(snapshots[0],snapshots[1])

    def test_invalid_options(self):
        self.rom.write_bytes(cartridge(bytes.fromhex('18fe')))
        for args in [('--trace-nop-run','0'),('--trace-nop-run','-1'),('--trace-nop-run','1x'),
                     ('--trace-nop-run','1','--trace-nop-run','2'),('--trace-mmm01',),
                     ('--trace-mmm01',str(self.root))]:
            r=subprocess.run([RUNNER,str(self.rom),*args],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(r.returncode,0,args)

if __name__=='__main__':
    unittest.main()
