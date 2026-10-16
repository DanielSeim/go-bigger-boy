#!/usr/bin/env python3
"""Contract tests for external-suite discovery and classification."""

from __future__ import annotations

import importlib.util
import json
import hashlib
import os
import shutil
import subprocess
from unittest.mock import patch
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parent))
SCRIPT = Path(__file__).with_name("run_external_suites.py")
SPEC = importlib.util.spec_from_file_location("run_external_suites", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


MANIFEST = json.loads(
    Path(__file__).with_name("external_suite_manifest.json").read_text())


class ExternalSuiteDiscoveryTests(unittest.TestCase):
    def test_bullygb_ci_download_matches_manifest_pin(self):
        config = MANIFEST['suites']['bullygb']
        workflow = (Path(__file__).resolve().parents[1] / '.github/workflows/desktop-builds.yml').read_text()
        self.assertIn(config['rom_url'], workflow)
        self.assertIn(config['rom_sha256'] + '  test-roms/bullygb/bully.gb', workflow)
        self.assertRegex(config['commit'], r'^[0-9a-f]{40}$')
        self.assertRegex(config['rom_sha256'], r'^[0-9a-f]{64}$')
        self.assertEqual(config['gate'], 'research')

    def test_bullygb_discovery_requires_pinned_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / 'bully.gb'
            config = dict(MANIFEST['suites']['bullygb'])
            self.assertTrue(all(c.status == 'fail' for c in MODULE.bullygb_cases(root, config)))
            rom.write_bytes(b'authored fixture')
            self.assertTrue(all(c.status == 'fail' for c in MODULE.bullygb_cases(root, config)))
            config['rom_sha256'] = hashlib.sha256(rom.read_bytes()).hexdigest()
            cases = MODULE.bullygb_cases(root, config)
            self.assertEqual(len(cases), 6)
            self.assertTrue(all(c.kind == 'bullygb' and c.status == 'pending' for c in cases))
            case = cases[0]
            for output, exit_code, status in (('PASS (BullyGB) result=All tests OK!\n', 0, 'pass'),
                    ('Captured frame 600', 0, 'fail'), ('FAIL (BullyGB) result=Invalid initial DIV', 1, 'fail')):
                with patch.object(MODULE.subprocess, 'run', return_value=
                        subprocess.CompletedProcess([], exit_code, output)) as run:
                    actual = MODULE.run_case(case, Path('/runner'), root / 'logs')
                    self.assertEqual(actual.status, status)
                    command = run.call_args.args[0]
                    self.assertEqual(command[command.index('--protocol') + 1], 'bullygb')

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

    def test_samesuite_revision_mapping_is_exact(self):
        expected = {
            'clock-cgb0.gb': (['cgb0'], []),
            'clock-cgbB.gb': ([], ['cgb-b']),
            'clock-cgb0B.gb': (['cgb0'], ['cgb-b']),
            'clock-cgb0BC.gb': (['cgb0', 'cgb-c'], ['cgb-b']),
            'clock-cgbBCE.gb': (['cgb-c', 'cgb-e'], ['cgb-b']),
            'clock-cgbDE.gb': (['cgb-e'], ['cgb-d']),
            'clock-cgbE.gb': (['cgb-e'], []),
            'clock-A.gb': (['agb'], []),
            'clock-future.gb': ([], ['unknown-future']),
            'div_write_trigger.gb': (['dmg'], []),
            'clock.gb': (['cgb-e'], []),
        }
        for name, (models, unsupported) in expected.items():
            with self.subTest(name=name):
                result = MODULE.samesuite_coverage(name)
                self.assertEqual(list(result.models), models)
                self.assertEqual(list(result.unsupported), unsupported)
                self.assertEqual(MODULE.samesuite_apu_models(name), models)
                cli = subprocess.run([sys.executable, str(Path(__file__).with_name('samesuite_models.py')), name],
                                     capture_output=True, text=True, check=True)
                self.assertEqual(cli.stdout.strip(), ';'.join((*models,
                    *('unsupported-' + r for r in unsupported))))


    def test_samesuite_unsupported_revisions_stay_visible(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'apu').mkdir()
            for name in ('clock-cgb0B.gb', 'clock-cgbB.gb', 'clock-A.gb'):
                (root / 'apu' / name).write_bytes(b'fixture')
            cases = MODULE.samesuite_cases(root, MANIFEST['suites']['same-suite'])
            self.assertEqual({c.model for c in cases if c.status == 'pending'}, {'cgb0', 'agb'})
            unsupported = [c for c in cases if c.status == 'unsupported']
            self.assertEqual(len(unsupported), 2)
            for case in unsupported:
                with patch.object(MODULE.subprocess, 'run') as run:
                    self.assertEqual(MODULE.run_case(case, Path('/unused'), root), case)
                    run.assert_not_called()

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ctest'), 'requires CMake/CTest')
    def test_direct_ctest_and_report_select_identical_apu_profiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            suite = root / 'suite with spaces'
            (suite / 'apu').mkdir(parents=True)
            for name in ('clock-cgb0B.gb', 'clock-cgb0BC.gb', 'clock-cgbB.gb',
                         'clock-cgbDE.gb', 'clock-A.gb', 'clock-future.gb', 'clock.gb'):
                (suite / 'apu' / name).write_bytes(b'fixture')
            source = root / 'source'
            source.mkdir()
            cmake = Path(__file__).resolve().parents[1] / 'cmake/ConformanceTests.cmake'
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(models NONE)\nenable_testing()\n'
                f'set(Python3_EXECUTABLE [==[{Path(sys.executable).as_posix()}]==])\n'
                f'set(GAMEBOY_SAMESUITE_DIR [==[{suite.as_posix()}]==])\n'
                'add_executable(gameboy_test_runner IMPORTED GLOBAL)\n'
                f'set_target_properties(gameboy_test_runner PROPERTIES IMPORTED_LOCATION '
                f'[==[{Path(sys.executable).as_posix()}]==])\n'
                f'include([==[{cmake.as_posix()}]==])\n')
            subprocess.run(['cmake', '-S', str(source), '-B', str(root / 'build')],
                           capture_output=True, text=True, check=True, timeout=60)
            output = subprocess.check_output(['ctest', '--test-dir', str(root / 'build'),
                '--show-only=json-v1', '-C', 'Release'], text=True, timeout=30)
            tests = json.loads(output)['tests']
            cases = MODULE.samesuite_cases(suite, MANIFEST['suites']['same-suite'])
            expected = {(str(c.rom.resolve()), c.model) for c in cases if c.status == 'pending'}
            running = [t for t in tests if '--model' in t.get('command', [])]
            actual = {(str(Path(t['command'][1]).resolve()),
                       t['command'][t['command'].index('--model') + 1]) for t in running}
            self.assertEqual(expected, actual)
            self.assertEqual(len(running), len(expected))
            skipped = [t for t in tests if t not in running]
            self.assertEqual(len(skipped), sum(c.status == 'unsupported' for c in cases))
            self.assertTrue(all(any(p['name'] == 'SKIP_REGULAR_EXPRESSION' and
                p['value'] == ['UNSUPPORTED'] for p in t['properties']) for t in skipped))
            self.assertEqual(len({t['name'] for t in tests}), len(tests))

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

    def test_runner_summary_prefers_result_over_diagnostic_tail(self) -> None:
        output = "FAIL (Mooneye result registers)\nPC=1234\nAPU PCM12=00 PCM34=00\n"
        self.assertEqual(
            MODULE.summarize_runner_output(output, 1),
            "FAIL (Mooneye result registers)",
        )

    def test_runner_summary_reports_timeout_exit(self) -> None:
        self.assertEqual(
            MODULE.summarize_runner_output("TIMEOUT after reaching the cycle limit\n", 2),
            "TIMEOUT after reaching the cycle limit",
        )

    def test_runner_summary_includes_structured_result_state(self) -> None:
        output = ("FAIL (Mooneye result registers)\n"
                  "RESULT expected_registers=B=03,C=05,D=08,E=0d,H=15,L=22 "
                  "observed_registers=B=42,C=42,D=42,E=42,H=42,L=42\n")
        summary = MODULE.summarize_runner_output(output, 1)
        self.assertIn("expected_registers=B=03", summary)
        self.assertIn("observed_registers=B=42", summary)

    def test_failure_clusters_group_external_subsystems(self) -> None:
        make_case = lambda suite, case_id: MODULE.Case(
            suite, case_id, None, None, None, False, "machine", 1,
            "research", "fail")
        self.assertEqual(
            MODULE.failure_cluster(make_case("age-test-roms", "m3-bg-scx_x")),
            "ppu/mode3-background")
        self.assertEqual(
            MODULE.failure_cluster(make_case("same-suite", "apu_channel_2_x")),
            "apu/channel_2")


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_BULLYGB_ROM_DIR'),
                     'set GBB_TEST_RUNNER and GBB_BULLYGB_ROM_DIR for the pinned external ROM')
class BullyGBNativeTests(unittest.TestCase):
    def test_aggregate_results_on_supported_profiles(self):
        config = MANIFEST['suites']['bullygb']
        cases = MODULE.bullygb_cases(Path(os.environ['GBB_BULLYGB_ROM_DIR']), config)
        self.assertTrue(all(c.status == 'pending' for c in cases), 'requires exact pinned ROM hash')
        with tempfile.TemporaryDirectory() as tmp:
            for case in cases:
                result = MODULE.run_case(case, Path(os.environ['GBB_TEST_RUNNER']).resolve(), Path(tmp))
                if case.model == 'cgb0':
                    # The same failure is independently observed with original
                    # CGB0 boot + pinned SameBoy. Do not falsify that revision.
                    self.assertEqual(result.status, 'fail')
                    self.assertIn('Invalid initial   DIV', result.detail)
                else:
                    self.assertEqual(result.status, 'pass', result.detail)


if __name__ == "__main__":
    unittest.main()
