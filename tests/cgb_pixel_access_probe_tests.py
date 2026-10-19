# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_pixel_access_probe as probe

ADAPTER = Path(sys.argv.pop(1)).resolve()


class PixelAccessContracts(unittest.TestCase):
    def test_owned_rom_grid_and_headers(self):
        cases = probe.scenarios()
        self.assertEqual(len(cases), 176)
        roms = [probe.probe_rom(c) for c in cases]
        self.assertEqual(len(set(roms)), 176)
        for case, rom in zip(cases, roms):
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], 0xc0)
            checksum = 0
            for byte in rom[0x134:0x14d]:
                checksum = (checksum - byte - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
            op = probe.OPERATIONS[case['operation']]
            pc = probe.operation_pc(case)
            self.assertEqual(rom[pc:pc + len(op)], op)

    def trace(self, rom, boot, enabled=True):
        result = subprocess.run([str(ADAPTER), str(rom), str(boot)] + ([] if enabled else ['--end-only']),
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_runtime_snapshots_preserve_complete_serialized_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot, rom = root / 'boot.bin', root / 'test.gbc'
            boot.write_bytes(probe.anchor.owned_boot())
            for speed in (False, True):
                for operation in probe.OPERATIONS:
                    candidates = [c for c in probe.scenarios() if c['speed'] == speed and c['operation'] == operation]
                    # Include the start and end windows to exercise allowed and blocked accesses.
                    accepted = set()
                    for case in (candidates[10], candidates[-1]):
                        with self.subTest(case=case['name']):
                            rom.write_bytes(probe.probe_rom(case))
                            observed = probe.validate_trace(self.trace(rom, boot), case, 'gbb')
                            control = probe.validate_trace(self.trace(rom, boot, False), case, 'gbb', False)
                            self.assertEqual(probe.final_state(observed), probe.final_state(control))
                            accepted.add(probe.witness(observed, case)['accepted'])
                    self.assertEqual(accepted, {False, True})

    def test_invalid_metadata_and_overlong_traces_fail_closed(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-pixel-access.json').read_text())
        example = evidence['examples'][0]
        case = next(c for c in probe.scenarios() if c['name'] == example['scenario'])
        trace = example['trace']
        probe.validate_trace(trace, case, trace['core'])
        for key, value in (('complete', False), ('qualification', True), ('model', 'cgb-c'),
                           ('anchor', 'lcd_on'), ('state_bytes', 0), ('clock_unit', 'cpu-t')):
            changed = copy.deepcopy(trace)
            changed[key] = value
            with self.assertRaises(ValueError):
                probe.validate_trace(changed, case, trace['core'])
        for change in ('frontier', 'clock', 'pc', 'overflow'):
            changed = copy.deepcopy(trace)
            if change == 'frontier': changed['samples'][2]['frontier'] = -2
            elif change == 'clock': changed['samples'][2]['clock'] = changed['samples'][1]['clock']
            elif change == 'pc': changed['samples'][0]['pc'] = 0
            else: changed['samples'] *= 100
            with self.assertRaises(ValueError):
                probe.validate_trace(changed, case, trace['core'])

    def test_runtime_limit_and_cli_failures_are_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom, boot = root / 'loop.gbc', root / 'boot.bin'
            case = probe.scenarios()[0]
            data = bytearray(probe.probe_rom(case))
            data[0x1000:0x1003] = bytes.fromhex('c3 00 10')
            rom.write_bytes(data)
            boot.write_bytes(probe.anchor.owned_boot())
            result = subprocess.run([str(ADAPTER), str(rom), str(boot)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 3)
            trace = json.loads(result.stdout)
            self.assertFalse(trace['complete'])
            self.assertLessEqual(len(trace['samples']), 513)
            self.assertLess(len(result.stdout), 200000)
            result = subprocess.run([str(ADAPTER), str(rom), str(boot), '--bad'], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 2)

    def test_recorded_provenance_and_complete_case_grid(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-pixel-access.json').read_text())
        self.assertFalse(evidence['qualification'])
        for path, digest in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest)
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.anchor.owned_boot()).hexdigest())
        cases = {c['name']: c for c in probe.scenarios()}
        rows = evidence['rows']
        self.assertEqual(len(rows), len(cases))
        self.assertEqual({r['scenario'] for r in rows}, set(cases))
        for row in rows:
            self.assertEqual(row['rom_sha256'], hashlib.sha256(probe.probe_rom(cases[row['scenario']])).hexdigest())
            a, b = row['captures']['gbb'], row['captures']['sameboy']
            self.assertEqual(row['architectural_match'], (a['value'], a['accepted']) == (b['value'], b['accepted']))
            for capture in (a, b):
                self.assertTrue(capture['repeatable'])
                self.assertTrue(capture['snapshot_equivalent'])
                self.assertTrue(capture['padding_equivalent'])
                self.assertIs(type(capture['accepted']), bool)
                for event in ('line0', 'line1'):
                    for interval in capture[event].values():
                        if interval is not None:
                            self.assertEqual(len(interval), 2)
                            self.assertLess(interval[0], interval[1])
        self.assertEqual(sum(row['architectural_match'] for row in rows), 166)
        quiet = evidence['rejected_quiet_calibration']
        self.assertFalse(quiet['accepted'])
        self.assertEqual(quiet['rom_sha256'], hashlib.sha256(probe.quiet_calibration_rom()).hexdigest())
        self.assertEqual(quiet['snapshot_final'], quiet['control_final'])
        self.assertNotEqual(quiet['snapshot_state_fnv1a64'], quiet['control_state_fnv1a64'])
        for example in evidence['examples']:
            case = cases[example['scenario']]
            trace = probe.validate_trace(example['trace'], case, example['trace']['core'])
            self.assertEqual(probe.witness(trace, case),
                             {k: v for k, v in next(r for r in rows if r['scenario'] == case['name'])['captures'][trace['core']].items()
                              if k not in ('repeatable', 'snapshot_equivalent', 'nop_control_value', 'padding_equivalent')})

    def test_missing_or_mismatched_cases_cannot_pass(self):
        self.assertFalse(probe.passes({'rows': []}))
        self.assertFalse(probe.passes({}))
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-pixel-access.json').read_text())
        self.assertFalse(probe.passes(evidence))
        changed = copy.deepcopy(evidence)
        for row in changed['rows']:
            for key in ('architectural_match', 'pixel_timeline_match', 'mode_timeline_match'):
                row[key] = True
            for capture in row['captures'].values():
                capture['padding_equivalent'] = True
                capture['accepted'] = True
        self.assertTrue(probe.passes(changed))
        changed['rows'][0]['captures']['gbb']['snapshot_equivalent'] = False
        self.assertFalse(probe.passes(changed))


if __name__ == '__main__':
    unittest.main()
