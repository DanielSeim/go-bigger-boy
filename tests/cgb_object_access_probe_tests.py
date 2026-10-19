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
import cgb_object_access_probe as probe

ADAPTER = Path(sys.argv.pop(1)).resolve()
FIXTURE = ROOT / 'tests/fixtures/external-reference/cgb-object-access.json'


class ObjectAccessContracts(unittest.TestCase):
    def test_owned_grid_and_headers(self):
        cases = probe.scenarios()
        self.assertEqual(len(cases), 512)
        roms = [probe.probe_rom(c) for c in cases]
        self.assertEqual(len(set(roms)), 512)
        for case, rom in zip(cases, roms):
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], 0xc0)
            self.assertEqual(rom[0x147], 0)
            check = 0
            for value in rom[0x134:0x14d]:
                check = (check - value - 1) & 255
            self.assertEqual(rom[0x14d], check)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'), sum(rom[:0x14e] + rom[0x150:]) & 65535)
            self.assertEqual(sum(cycles for _, cycles in probe.program(case)), 48 + 4*case['pad'])
            self.assertLess(len(probe.program(case)) + 4, 513)

    def test_live_accesses_and_mid_toggle_observer_equivalence(self):
        evidence = json.loads(FIXTURE.read_text())
        expected = {r['scenario']: r['captures']['gbb']['value'] for r in evidence['rows']}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot, rom = root / 'boot.bin', root / 'probe.gbc'
            boot.write_bytes(probe.phase.access.anchor.owned_boot())
            for case in [c for c in probe.scenarios() if c['name'].endswith('dot264')]:
                with self.subTest(case=case['name']):
                    rom.write_bytes(probe.probe_rom(case))
                    traces = []
                    for enabled in (True, False):
                        result = subprocess.run([str(ADAPTER), str(rom), str(boot)] +
                                                ([] if enabled else ['--end-only']),
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        traces.append(probe.validate_trace(json.loads(result.stdout), case, 'gbb', enabled))
                    self.assertEqual(probe.phase.access.final_state(traces[0]), probe.phase.access.final_state(traces[1]))
                    self.assertEqual(probe.value(traces[0], case), expected[case['name']])

    def test_metadata_clock_and_bounds_fail_closed(self):
        example = json.loads(FIXTURE.read_text())['examples'][0]
        case = next(c for c in probe.scenarios() if c['name'] == example['scenario'])
        trace = example['trace']
        probe.validate_trace(trace, case, trace['core'])
        for key, value in (('model', 'cgb0'), ('qualification', True), ('complete', False), ('state_bytes', 0)):
            bad = copy.deepcopy(trace)
            bad[key] = value
            with self.assertRaises(ValueError):
                probe.validate_trace(bad, case, trace['core'])
        bad = copy.deepcopy(trace)
        bad['samples'][4]['clock'] += 1
        with self.assertRaises(ValueError):
            probe.validate_trace(bad, case, trace['core'])
        bad = copy.deepcopy(trace)
        bad['samples'] *= 10
        with self.assertRaises(ValueError):
            probe.validate_trace(bad, case, trace['core'])

    def test_current_and_rejected_evidence_preserves_disagreements(self):
        evidence = json.loads(FIXTURE.read_text())
        self.assertFalse(evidence['qualification'])
        self.assertEqual(evidence['reference']['revision'], '208ba4afabffab9edde416f2dbb8ae459e34adb8')
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.phase.access.anchor.owned_boot()).hexdigest())
        for path, sha in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), sha, path)
        cases = {c['name']: c for c in probe.scenarios()}
        rows = evidence['rows']
        self.assertEqual(len(rows), 512)
        self.assertEqual({r['scenario'] for r in rows}, set(cases))
        before = {r['case']['name']: r for r in evidence['before']}
        self.assertEqual(sum(r['previous_match'] for r in before.values()), 402)
        self.assertEqual(sum(r['match'] for r in rows), 466)
        for row in rows:
            self.assertEqual(row['rom_sha256'], hashlib.sha256(probe.probe_rom(cases[row['scenario']])).hexdigest())
            self.assertEqual(row['match'], row['captures']['gbb']['value'] == row['captures']['sameboy']['value'])
            self.assertEqual(row['captures']['sameboy']['value'], before[row['scenario']]['values']['sameboy'])
            self.assertFalse(before[row['scenario']]['previous_match'] and not row['match'])
            for capture in row['captures'].values():
                self.assertTrue(capture['repeatable'])
                self.assertTrue(capture['snapshot_equivalent'])
        rejected = evidence['rejected_cancellation_change']
        self.assertEqual(rejected['matches'], 488)
        self.assertEqual(len(rejected['new_regressions']), 4)
        self.assertTrue(all(r['previous_match'] and not r['match'] for r in rejected['new_regressions']))
        self.assertEqual(sum(r['captures']['sameboy']['value'] == 0 for r in rows), 8)
        for example in evidence['examples']:
            case = cases[example['scenario']]
            trace = example['trace']
            probe.validate_trace(trace, case, trace['core'])
            row = next(r for r in rows if r['scenario'] == example['scenario'])
            self.assertEqual(probe.value(trace, case), row['captures'][trace['core']]['value'])

    def test_missing_cases_and_known_mismatches_are_not_validation_success(self):
        report = json.loads(FIXTURE.read_text())
        self.assertFalse(probe.passes(report))
        good = copy.deepcopy(report)
        for row in good['rows']:
            row['match'] = True
            row['captures']['gbb']['value'] = row['captures']['sameboy']['value']
        self.assertTrue(probe.passes(good))
        for action in ('missing', 'duplicate', 'observer'):
            bad = copy.deepcopy(good)
            if action == 'missing': bad['rows'].pop()
            elif action == 'duplicate': bad['rows'][-1] = bad['rows'][0]
            else: bad['rows'][0]['captures']['gbb']['snapshot_equivalent'] = False
            self.assertFalse(probe.passes(bad))


if __name__ == '__main__':
    unittest.main()
