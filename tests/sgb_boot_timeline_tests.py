#!/usr/bin/env python3
"""Original ROM-free boot timeline and input-provenance contracts."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_sgb_boot_timeline import compare, validate
from capture_sgb_libretro_audio import BUTTON_IDS, native_button_mask

COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None


def capture(source):
    rows = [("I", 0xaabb, 0, 0), ("U", 0x400, 20, 123),
            ("N", 128, 1000, 0), ("S", 0x1000000, 0, 0)]
    return {"format": "gbb-sgb-boot-timeline-v1", "source": source,
            "input_mode": "gb-lcd-frame-held-v1",
            "master_hz": 20, "apu_half_hz": 2,
            "events": [dict(kind=k, master_clock_snapshot=100 + i * 20,
                            spc_half_clock_snapshot=i * 2, value=v, count=c,
                            digest_fnv64=d) for i, (k, v, c, d) in enumerate(rows)]}


class Contracts(unittest.TestCase):
    def test_matching_preconditions(self):
        self.assertTrue(compare(capture("gbb"), capture("reference"))["input_and_upload_preconditions_met"])

    def test_legacy_or_unmarked_inputs_are_not_accepted(self):
        for mode in ("legacy-libretro-run-v1", None):
            r = capture("reference")
            r["input_mode"] = mode
            self.assertFalse(compare(capture("gbb"), r)["input_and_upload_preconditions_met"])

    def test_input_mismatch_is_not_hidden_by_alignment(self):
        r = capture("reference")
        r["events"][2]["count"] = 337
        result = compare(capture("gbb"), r)
        self.assertFalse(result["input_and_upload_preconditions_met"])
        self.assertEqual(result["first_input_mismatch"]["reference_frame_mask"], (337, 128))

    def test_masks_and_partial_histories(self):
        r = capture("reference")
        r["events"][2]["value"] = 0
        self.assertIsNotNone(compare(capture("gbb"), r)["first_input_mismatch"])
        r["events"].pop(2)
        result = compare(capture("gbb"), r)
        self.assertFalse(result["input_and_upload_preconditions_met"])
        self.assertEqual(result["input_counts"], [1, 0])

    def test_upload_digest_size_and_destination(self):
        for field in ("value", "count", "digest_fnv64"):
            r = capture("reference")
            r["events"][1][field] += 1
            self.assertFalse(compare(capture("gbb"), r)["upload_fingerprints_match"])

    def test_clock_rates_not_normalized_away(self):
        r = capture("reference")
        r["master_hz"] = 10
        self.assertEqual(compare(capture("gbb"), r)["landmarks"][0]["reference_minus_gbb_seconds"], 5)

    def test_reject_corrupt_provenance(self):
        for field, value in (("source", "gbb"), ("master_hz", 0), ("apu_half_hz", True),
                             ("host_bus_timing", 1), ("external_boot_reset", 1), ("events", [])):
            r = capture("reference")
            r[field] = value
            with self.assertRaises(ValueError):
                validate(r, "reference")
        for field in ("kind", "count", "digest_fnv64"):
            r = capture("reference")
            r["events"][0][field] = None
            with self.assertRaises(ValueError):
                validate(r, "reference")

    def test_libretro_mask_conversion_all_buttons(self):
        for bit, name in enumerate(("right", "left", "up", "down", "a", "b", "select", "start")):
            self.assertEqual(native_button_mask(1 << BUTTON_IDS[name]), 1 << bit)
        self.assertEqual(native_button_mask(0), 0)
        self.assertEqual(native_button_mask(sum(1 << i for i in BUTTON_IDS.values())), 255)

    @unittest.skipUnless(COMPILER, "standalone probe protocol requires a C++ compiler")
    def test_original_reference_probe_protocol_and_overflow(self):
        # Compile only our added probe header with clock stubs, not bsnes or firmware.
        patch = (Path(__file__).resolve().parents[1] / "scripts/patches/bsnes-05439f9-sgb-boot-timeline.patch").read_text()
        section = patch.split("+++ b/bsnes/sfc/dsp/gbb_boot_probe.hpp\n", 1)[1]
        header = "\n".join(line[1:] for line in section.splitlines() if line.startswith("+"))
        source = """
#include <cassert>
#include <cstdint>
#include <vector>
using uint64 = uint64_t;
using uint8 = uint8_t;
struct { uint64 diagnosticHalfClocks = 4808; } smp;
struct { uint64 diagnosticMasterClocks() { return 50000; } } cpu;
""" + header + """
int main() {
  assert(gbb_reference_sgb_boot_version() == 1);
  gbb_reference_sgb_boot_enable(1);
  diagnosticBootHost(0x2142, 0); diagnosticBootHost(0x2143, 4);
  diagnosticBootHost(0x2141, 1); diagnosticBootHost(0x2140, 0xcc);
  uint64 expected = 14695981039346656037ull;
  for(unsigned i = 0; i < 300; ++i) {
    diagnosticBootHost(0x2141, i & 255);
    diagnosticBootHost(0x2140, i & 255);
    expected = (expected ^ (i & 255)) * 1099511628211ull;
  }
  diagnosticBootHost(0x2141, 0); diagnosticBootHost(0x2140, 301 & 255);
  uint64 e[7];
  assert(gbb_reference_sgb_boot_count() == 3);
  assert(gbb_reference_sgb_boot_copy(1, e) == 1);
  assert(e[0] == 'U' && e[3] == 0x400 && e[4] == 300 && e[5] == expected);
  assert(e[2] == 4808 && e[6] == 0);
  assert(gbb_reference_sgb_boot_copy(2, e) == 1 && e[0] == 'E');
  assert(gbb_reference_sgb_boot_copy(3, e) == 0);
  assert(gbb_reference_sgb_boot_copy(0, nullptr) == 0);
  gbb_reference_sgb_boot_enable(1);
  for(unsigned i = 0; i < 128; ++i) gbb_reference_sgb_record_boot('C', i, 1, 0, 0);
  assert(gbb_reference_sgb_boot_count() == 128);
  gbb_reference_sgb_record_boot('C', 129, 1, 0, 0);
  assert(gbb_reference_sgb_boot_count() == 0xffffffffu);
  gbb_reference_sgb_boot_enable(0);
  gbb_reference_sgb_record_boot('C', 130, 1, 0, 0);
  assert(gbb_reference_sgb_boot_count() == 0);
}
"""
        with tempfile.TemporaryDirectory(prefix="gbb-boot-probe-") as directory:
            root = Path(directory)
            cpp = root / "probe.cpp"
            exe = root / ("probe.exe" if os.name == "nt" else "probe")
            cpp.write_text(source)
            msvc = Path(COMPILER).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
            command = ([COMPILER, "/nologo", "/std:c++17", str(cpp), "/Fe:" + str(exe)] if msvc else
                       [COMPILER, "-std=c++17", str(cpp), "-o", str(exe)])
            subprocess.run(command, check=True, capture_output=True, timeout=30, cwd=root)
            subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
