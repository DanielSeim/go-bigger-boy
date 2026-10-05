#!/usr/bin/env python3
"""ROM-free tests for the execution-only SGB comparison tool."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from validate_sgb_boot import compare, compare_independent, packets


def fixture():
    wire=[]
    data=[]
    for p in range(6):
        packet=bytes([0xf1+2*p]+[0]*15)
        data.extend(packet)
        wire.extend([[0,0xff00,0x30],[0,0xff00,0]])
        for byte in packet:
            for bit in range(8):
                wire.extend([[0,0xff00,0x30],[0,0xff00,0x10 if byte & (1<<bit) else 0x20]])
        wire.extend([[0,0xff00,0x30],[0,0xff00,0x20],[0,0xff00,0x30]])
    return {"handoff":{"cpu":{"a":1},"io":[0]*128,"wram":data,
                       "cycles":1,"divider_counter":2,"ppu_dot":3,"ppu_scanline":153,"ppu_mode":1,
                       "ie":0,"serial_phase":0,"serial_bits":0,"apu_clocks":[0]*18},
            "boot_writes":wire}


class Tests(unittest.TestCase):
    def test_same_contract(self):
        a=fixture()
        self.assertEqual(len(packets(a["boot_writes"])),6)
        self.assertEqual(compare(a,a)["contract"],"pass")

    def test_phase_mismatches_fail(self):
        for field in ("cycles", "divider_counter", "ppu_dot", "ppu_scanline", "ppu_mode",
                      "serial_phase", "serial_bits"):
            with self.subTest(field=field):
                a=fixture(); b=copy.deepcopy(a); b["handoff"][field]+=1
                with self.assertRaisesRegex(AssertionError,"phase"):
                    compare(a,b)
        a=fixture(); b=copy.deepcopy(a); b["handoff"]["apu_clocks"][0]+=1
        with self.assertRaisesRegex(AssertionError,"apu_clocks"):
            compare(a,b)

    def test_timeline_mismatch(self):
        a=fixture(); b=copy.deepcopy(a); b["boot_writes"][5][0]+=4
        with self.assertRaisesRegex(AssertionError,"timeline"):
            compare(a,b)

    def test_exact_contract_report(self):
        a=fixture()
        self.assertTrue(compare(a,a)["cycle_exact"])

    def test_independent_contract_and_time_origin(self):
        a={k:0 for k in ("af","bc","de","hl","sp","pc","ly","div","stat","divider_counter")}
        a.update(apu_phase=[0]*20,ppu_phase=[153,17,2,0],writes=[[32,0xff00,0x30],[100,0xff26,0x80]],
                 ly_reads=[[150,182,144,2]])
        b=copy.deepcopy(a)
        for write in b["writes"]: write[0]+=4000
        for read in b["ly_reads"]: read[0]+=4000; read[1]+=4000
        self.assertEqual(compare_independent(a,b)["contract"],"pass")
        b["writes"][1][0]+=4
        with self.assertRaisesRegex(AssertionError,"timeline"):
            compare_independent(a,b)
        b=copy.deepcopy(a); b["apu_phase"][0]+=1
        with self.assertRaisesRegex(AssertionError,"apu_phase"):
            compare_independent(a,b)

    def test_cpu_mismatch(self):
        a=fixture(); b=copy.deepcopy(a); b["handoff"]["cpu"]["a"]=255
        with self.assertRaisesRegex(AssertionError,"CPU"):
            compare(a,b)

    def test_io_mismatch(self):
        a=fixture(); b=copy.deepcopy(a); b["handoff"]["io"][0x40]=0x91
        with self.assertRaisesRegex(AssertionError,"IO"):
            compare(a,b)

    def test_buffer_mismatch(self):
        a=fixture(); b=copy.deepcopy(a); b["handoff"]["wram"][17]=1
        with self.assertRaisesRegex(AssertionError,"buffer"):
            compare(a,b)

    def test_wire_mismatch(self):
        a=fixture(); b=copy.deepcopy(a); b["boot_writes"][3][2]=0x20
        with self.assertRaisesRegex(AssertionError,"transmitted"):
            compare(a,b)

    def test_invalid_stop(self):
        a=fixture(); a["boot_writes"][-2][2]=0x10
        with self.assertRaisesRegex(AssertionError,"stop"):
            packets(a["boot_writes"])

    def test_missing_packet(self):
        with self.assertRaisesRegex(AssertionError,"incomplete"):
            packets(fixture()["boot_writes"][:260])


if __name__=="__main__":
    unittest.main()
