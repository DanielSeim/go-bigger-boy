#!/usr/bin/env python3
"""ROM-free held-state, reset, controller clobber and ABI contracts."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from capture_sgb_libretro_audio import configure_frame_input, BUTTON_IDS, capture
COMPILER = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
SOURCE_PROBE = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None


class Function:
    def __init__(self, result=1):
        self.result, self.calls = result, []
    def __call__(self, *args):
        self.calls.append(args)
        return self.result


class Core:
    def __init__(self):
        for name in ("version", "enable", "add", "applied"):
            setattr(self, "gbb_reference_sgb_frame_input_" + name, Function())


class Contracts(unittest.TestCase):
    @unittest.skipUnless(SOURCE_PROBE, "requires diagnostic source probe")
    def test_external_boot_starts_with_lcd_off_and_no_phantom_frames(self):
        with tempfile.TemporaryDirectory(prefix="gbb-icd-cold-boot-") as directory:
            root = Path(directory)
            rom, boot, script = root / "original.gb", root / "original.boot", root / "input.script"
            rom.write_bytes(bytes(32768))
            # Original boot waits 140k GB clocks before enabling the LCD.
            code = bytes([0x3e, 0x10, 0xe0, 0x00, 0xf3, 0x01, 0x88, 0x13,
                          0x0b, 0x78, 0xb1, 0x20, 0xfb, 0x3e, 0x91, 0xe0, 0x40,
                          0xc3, 0x11, 0x00])
            boot.write_bytes(code + bytes(256 - len(code)))
            script.write_text("GBB SGB input v1\n0 start\n1 none\n2 a\n")
            result = subprocess.run([SOURCE_PROBE, str(rom), str(boot), str(script), "--cold-reset"],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    @unittest.skipUnless(SOURCE_PROBE, "requires diagnostic source probe")
    def test_actual_icd_source_held_release_and_reset(self):
        # Entire ROM and 256-byte boot are original; no commercial firmware.
        with tempfile.TemporaryDirectory(prefix="gbb-icd-original-input-") as directory:
            root = Path(directory)
            rom, boot, script = root / "original.gb", root / "original.boot", root / "input.script"
            rom.write_bytes(bytes(32768))
            boot.write_bytes(bytes([0x3e, 0x10, 0xe0, 0x00, 0x3e, 0x91, 0xe0, 0x40,
                                   0xc3, 0x08, 0x00]) + bytes(245))
            script.write_text("GBB SGB input v1\n0 start\n1 none\n2 a\n")
            result = subprocess.run([SOURCE_PROBE, str(rom), str(boot), str(script)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    def test_queue_masks_and_disable_sticky_state(self):
        core = Core()
        configure_frame_input(core, [(0, 1 << BUTTON_IDS["start"]), (4, 0)], True)
        self.assertEqual(core.gbb_reference_sgb_frame_input_add.calls, [(0, 128), (4, 0)])
        configure_frame_input(core, [], False)
        self.assertEqual(core.gbb_reference_sgb_frame_input_enable.calls, [(1,), (0,)])

    def test_missing_version_and_rejected_queue(self):
        configure_frame_input(object(), [], False)
        with self.assertRaisesRegex(RuntimeError, "version-1"):
            configure_frame_input(object(), [], True)
        for symbol in ("version", "enable", "add"):
            core = Core()
            getattr(core, "gbb_reference_sgb_frame_input_" + symbol).result = 0
            with self.assertRaises(RuntimeError):
                configure_frame_input(core, [(1, 0)], True)

    def test_native_mode_forbids_estimated_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = [root / n for n in ("core", "game", "firmware")]
            for path in files:
                path.write_bytes(b"original dummy")
            for options in ({}, {"require_snes_only_probe": True},
                            {"require_snes_only_probe": True, "boot_timeline_output": root / "boot",
                             "input_script": root / "input", "input_offset": 1},
                            {"require_snes_only_probe": True, "boot_timeline_output": root / "boot",
                             "input_script": root / "input", "input_offset_changes": [(1, 1)]}):
                with self.assertRaisesRegex(ValueError, "without frame offsets"):
                    capture(*files, root, 1, root / "audio.wav", native_gb_input=True, **options)

    @unittest.skipUnless(COMPILER, "requires C++ compiler")
    def test_shared_policy_and_reference_patch_are_identical(self):
        patch = (ROOT / "scripts/patches/bsnes-05439f9-sgb-frame-input.patch").read_text()
        section = patch.split("+++ b/bsnes/sfc/coprocessor/icd/gbb_frame_input.hpp\n", 1)[1]
        added = "\n".join(line[1:] for line in section.splitlines() if line.startswith("+"))
        core = (ROOT / "include/gameboy/sgb_frame_input.hpp").read_text()
        # The implementation moved into the core. Ignore only its renamed
        # type/namespace, documentation and snapshot friendship; keep every
        # policy statement identical to the independently built reference.
        core = core.replace(
            "// Original held-frame input policy for deterministic firmware replays.\n"
            "// Not a frontend controller backend.",
            "// Original diagnostic input policy. Not a production controller backend.")
        core = core.replace("namespace gameboy", "namespace sgb_test")
        core = core.replace("class SgbFrameInput", "class FrameInput")
        core = core.replace("    friend class SgbHostStateCodec;\n", "")
        self.assertEqual(added.strip(), core.strip())
        source = r'''
#include "sgb_frame_input.hpp"
#include <cassert>
int main() {
  sgb_test::FrameInput q;
  assert(q.add(0, 0x80) && q.add(4, 0) && q.add(8, 0x13));
  assert(!q.add(8, 0) && !q.add(7, 0) && !q.add(9, 256) && !q.add(100001, 0));
  q.reset(); assert(q.held() == 0 && q.applied() == 0);
  assert(q.advance(0) && q.held() == 0x80);
  for(unsigned frame = 1; frame < 4; ++frame) {
    assert(!q.advance(frame) && q.held() == 0x80);
    // Reproduce the bug: a SNES poll says released; native mode must keep Start held.
    assert(q.controller(0, 0xff, true) == 0x7f);
    assert(q.controller(0, 0xff, false) == 0xff);
    assert(q.controller(1, 0xa5, true) == 0xa5);
  }
  assert(q.advance(4) && q.held() == 0);
  assert(q.controller(0, 0, true) == 0xff);
  assert(q.advance(8) && q.controller(0, 0xff, true) == 0xec);
  q.reset(); assert(q.applied() == 0 && q.held() == 0 && q.advance(0));
  q.clear(); assert(q.size() == 0 && !q.advance(0));
  for(unsigned i = 0; i < 1024; ++i) assert(q.add(i, i & 255));
  assert(!q.add(1024, 0));
  for(unsigned i = 0; i < 1024; ++i) assert(q.advance(i) && q.held() == (i & 255));
  assert(q.applied() == 1024 && !q.advance(1024));
}
'''
        with tempfile.TemporaryDirectory(prefix="gbb-frame-input-") as directory:
            root = Path(directory)
            cpp, exe = root / "input.cpp", root / ("input.exe" if os.name == "nt" else "input")
            cpp.write_text(source)
            include = str(ROOT / "tests/support")
            core_include = str(ROOT / "include")
            msvc = Path(COMPILER).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
            command = ([COMPILER, "/nologo", "/EHsc", "/std:c++17", "/I" + include,
                        "/I" + core_include, str(cpp), "/Fe:" + str(exe)] if msvc
                       else [COMPILER, "-std=c++17", "-I", include, "-I", core_include,
                             str(cpp), "-o", str(exe)])
            subprocess.run(command, check=True, capture_output=True, timeout=30, cwd=root)
            subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
