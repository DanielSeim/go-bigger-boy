#!/usr/bin/env python3
"""Contract tests for external-suite discovery and classification."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("run_external_suites.py")
SPEC = importlib.util.spec_from_file_location("run_external_suites", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


MANIFEST = json.loads(
    Path(__file__).with_name("external_suite_manifest.json").read_text())


class ExternalSuiteDiscoveryTests(unittest.TestCase):
    def test_age_model_suffixes_preserve_supported_profiles(self) -> None:
        self.assertEqual(MODULE.age_models("foo-dmgC-cgbBCE"),
                         (["dmg", "cgb-c", "cgb-e"], False))
        self.assertEqual(MODULE.age_models("foo-ncmBCE"),
                         (["cgb-c", "cgb-e"], True))
        self.assertEqual(MODULE.age_models("foo-cgbE"),
                         (["cgb-e"], False))

    def test_age_discovery_pairs_visual_references_with_models(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "age-test-roms"
            visual = root / "m3-bg-lcdc"
            visual.mkdir(parents=True)
            (visual / "m3-bg-lcdc.gb").write_bytes(b"rom")
            (visual / "m3-bg-lcdc-nocgb.gb").write_bytes(b"rom")
            (visual / "m3-bg-lcdc-cgbBCE.png").write_bytes(b"png")
            (visual / "m3-bg-lcdc-nocgb-ncmBCE.png").write_bytes(b"png")
            cases = MODULE.age_cases(root, MANIFEST["suites"]["age-test-roms"])
            self.assertEqual(len(cases), 4)
            self.assertEqual(
                {case.rom.name for case in cases},
                {"m3-bg-lcdc.gb", "m3-bg-lcdc-nocgb.gb"},
            )
            self.assertEqual(
                {(case.model, case.compatibility_colors) for case in cases},
                {("cgb-c", False), ("cgb-e", False),
                 ("cgb-c", True), ("cgb-e", True)},
            )

    def test_samesuite_classifies_sgb_as_informational(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "same-suite"
            for category, name in (
                    ("apu", "channel_1_align.gb"),
                    ("dma", "gdma_addr_mask.gb"),
                    ("interrupt", "ei_delay_halt.gb"),
                    ("ppu", "blocking_bgpi_increase.gb"),
                    ("sgb", "command_mlt_req.gb")):
                directory = root / category
                directory.mkdir(parents=True)
                (directory / name).write_bytes(b"rom")
            cases = MODULE.samesuite_cases(
                root, MANIFEST["suites"]["same-suite"])
            self.assertEqual(
                {(case.kind, case.status) for case in cases if case.suite == "same-suite"},
                {("machine", "pending"), ("informational", "info")},
            )
            self.assertTrue(any(case.model == "cgb-e" for case in cases))


if __name__ == "__main__":
    unittest.main()
