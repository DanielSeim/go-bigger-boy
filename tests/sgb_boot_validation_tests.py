#!/usr/bin/env python3
"""ROM-free tests for the execution-only SGB comparison tool."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from validate_sgb_boot import compare, packets


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
                       "cycles":1,"divider_counter":2,"ppu_dot":3,"ppu_mode":1},
            "boot_writes":wire}


class Tests(unittest.TestCase):
    def test_same_contract(self):
        a=fixture()
        self.assertEqual(len(packets(a["boot_writes"])),6)
        self.assertEqual(compare(a,a)["contract"],"pass")

    def test_timing_is_reported_not_claimed_exact(self):
        a=fixture(); b=copy.deepcopy(a)
        b["handoff"]["divider_counter"]+=1
        result=compare(a,b)
        self.assertFalse(result["cycle_exact"])
        self.assertNotEqual(result["replacement"],result["reference"])

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
