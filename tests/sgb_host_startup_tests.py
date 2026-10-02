#!/usr/bin/env python3
"""Original ROM-free host startup report and independent probe contracts."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from compare_sgb_host_startup import compare, validate

COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 else None
TRACER = sys.argv.pop(1) if len(sys.argv) > 1 else None


def capture(source):
    return {"format": "gbb-sgb-host-startup-v1", "source": source, "master_hz": 1000,
            "instructions": [[100 + 10 * i, 0x8000 + i, 0, 0, 0, 0x1FF, 0, 0, 0x34, 0, 100 + 10 * i]
                             for i in range(4)]}


class Contracts(unittest.TestCase):
    @unittest.skipUnless(TRACER, "requires diagnostic trace executable")
    def test_clock_profile_rejected_before_loading_roms(self):
        for options, message in ((["--apu-clock-hz", "1025280"], "requires cycle"),
                                 (["--apu-clock-hz", "1025281"], "divisible by 32"),
                                 (["--apu-clock-hz", "0"], "divisible by 32"),
                                 (["--apu-clock-hz", "1025280", "--apu-clock-hz", "1024000"],
                                  "unsupported synchronized GB option")):
            result = subprocess.run([TRACER, "absent-program", "absent-ipl", "--sync-gb-sgb2",
                                     "absent-game", "absent-boot", *options],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertIn(message, result.stderr)

    def test_matching_pc_is_not_matching_state(self):
        r = capture("reference")
        r["instructions"][1][2] = 3
        r["instructions"][3][1] += 1
        result = compare(capture("gbb"), r)
        self.assertEqual(result["matching_pc_prefix_length"], 3)
        self.assertEqual(result["first_register_state_mismatch"]["ordinal_zero_based"], 1)
        self.assertEqual(result["first_pc_mismatch"]["ordinal_zero_based"], 3)
        self.assertEqual(len(result["largest_instruction_interval_deltas"]), 2)

    def test_clock_stall_is_not_fitted_away(self):
        r = capture("reference")
        for row in r["instructions"][2:]:
            row[0] += 540000
        result = compare(capture("gbb"), r)
        self.assertEqual(result["largest_instruction_interval_deltas"][0]["ordinal_zero_based"], 1)
        self.assertEqual(result["largest_instruction_interval_deltas"][0]["reference_minus_gbb_seconds"], 540)
        self.assertIsNone(result["first_register_state_mismatch"])
        r["master_hz"] = 2000
        self.assertNotEqual(compare(capture("gbb"), r)["largest_instruction_interval_deltas"][0]
                            ["reference_minus_gbb_seconds"], 540)

    def test_shorter_capture_is_not_reported_as_equal(self):
        r = capture("reference")
        r["instructions"].pop()
        result = compare(capture("gbb"), r)
        self.assertEqual(result["instruction_counts"], [4, 3])
        self.assertEqual(result["matching_pc_prefix_length"], 3)

    def test_reject_corruption(self):
        for field, value in (("source", "reference"), ("master_hz", True),
                             ("instructions", []), ("ppu_dma_timing", 1),
                             ("host_bus_timing", 1), ("apu_half_hz", True),
                             ("apu_half_hz", 0),
                             ("dma_requests", [[0] * 6])):
            d = capture("gbb")
            d[field] = value
            with self.assertRaises(ValueError):
                validate(d, "gbb")
        for index, value in ((0, -1), (1, 1 << 24), (8, True), (9, 262), (10, 1364)):
            d = capture("gbb")
            d["instructions"][0][index] = value
            with self.assertRaises(ValueError):
                validate(d, "gbb")
        d = capture("gbb")
        d["instructions"][1][0] = d["instructions"][0][0]
        with self.assertRaises(ValueError):
            validate(d, "gbb")

    @unittest.skipUnless(COMPILER, "probe contract needs a C++ compiler")
    def test_original_probe_bounds_gating_and_reset(self):
        patch = (ROOT / "scripts/patches/bsnes-05439f9-sgb-host-startup.patch").read_text()
        section = patch.split("+++ b/bsnes/sfc/dsp/gbb_host_startup.hpp\n", 1)[1]
        header = "\n".join(line[1:] for line in section.splitlines() if line.startswith("+"))
        source = """
#include <array>
#include <vector>
#include <cassert>
#include <cstdint>
using uint64 = uint64_t;
using uint8 = uint8_t;
static unsigned diagnosticHostIplReads = 0;
static bool diagnosticBootTransfer = false;
struct Word { unsigned w = 0; };
struct { struct { struct { unsigned d = 0x8123; } pc;
  Word a, x, y, s, d; unsigned b = 0, p = 0x34; } r;
  uint64 diagnosticMasterClocks() { return 100; }
  unsigned vcounter() { return 0; } unsigned hcounter() { return 100; }
} cpu;
""" + header + """
int main() {
  assert(gbb_reference_sgb_host_startup_version() == 1);
  gbb_reference_sgb_host_startup_enable(1);
  gbb_reference_sgb_record_host_startup();
  assert(gbb_reference_sgb_host_startup_count() == 0);
  diagnosticHostIplReads = 3;
  gbb_reference_sgb_record_host_startup();
  uint64 row[11];
  assert(gbb_reference_sgb_host_startup_copy(0, row) == 1 && row[1] == 0x8123);
  assert(gbb_reference_sgb_host_startup_copy(1, row) == 0);
  assert(gbb_reference_sgb_host_startup_copy(0, nullptr) == 0);
  diagnosticBootTransfer = true;
  gbb_reference_sgb_record_host_startup();
  assert(gbb_reference_sgb_host_startup_count() == 1);
  diagnosticBootTransfer = false;
  for(unsigned i = 1; i < 131072; ++i) gbb_reference_sgb_record_host_startup();
  assert(gbb_reference_sgb_host_startup_count() == 131072);
  gbb_reference_sgb_record_host_startup();
  assert(gbb_reference_sgb_host_startup_count() == 0xffffffffu);
  gbb_reference_sgb_host_startup_enable(0);
  gbb_reference_sgb_record_host_startup();
  assert(gbb_reference_sgb_host_startup_count() == 0);
}
"""
        with tempfile.TemporaryDirectory(prefix="gbb-host-probe-") as directory:
            root = Path(directory)
            cpp, exe = root / "probe.cpp", root / ("probe.exe" if os.name == "nt" else "probe")
            cpp.write_text(source)
            msvc = Path(COMPILER).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
            command = ([COMPILER, "/nologo", "/std:c++17", str(cpp), "/Fe:" + str(exe)] if msvc else
                       [COMPILER, "-std=c++17", str(cpp), "-o", str(exe)])
            subprocess.run(command, cwd=root, check=True, capture_output=True, timeout=30)
            subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
