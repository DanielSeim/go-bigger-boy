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
import cgb_pixel_phase_probe as probe

ADAPTER = Path(sys.argv.pop(1)).resolve()
FIXTURE = ROOT / 'tests/fixtures/external-reference/cgb-pixel-phase.json'


class PixelPhaseContracts(unittest.TestCase):
    def test_owned_grid_headers_and_static_controls(self):
        cases = probe.scenarios()
        self.assertEqual(len(cases), 80)
        roms = [probe.probe_rom(c) for c in cases]
        self.assertEqual(len(set(roms)), 80)
        for case, rom in zip(cases, roms):
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], 0xc0)
            self.assertEqual(rom[0x147], 0)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'), sum(rom[:0x14e] + rom[0x150:]) & 65535)
            start = 0x2000 + case['phase']
            self.assertEqual(rom[start:start + probe.poll_count(case)], b'\x7e' * probe.poll_count(case))
            self.assertIn(bytes.fromhex('21 41 ff c3 00 10'), rom[0x150:0x1000])
            self.assertLess(probe.poll_count(case) + 6, 512)
        with self.assertRaises(ValueError):
            probe.probe_rom(dict(cases[0], scx=9))
        with self.assertRaises(ValueError):
            probe.probe_rom(cases[0], 'unsupported')

    def test_live_two_line_edges_and_serialized_observer_equivalence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot, rom = root / 'boot.bin', root / 'probe.gbc'
            boot.write_bytes(probe.access.anchor.owned_boot())
            cases = [c for c in probe.scenarios() if c['control'] in
                     ('background', 'window-left', 'object-middle', 'object-disabled-middle') and c['scx'] in (0, 7)]
            for case in cases:
                with self.subTest(case=case['name']):
                    rom.write_bytes(probe.probe_rom(case))
                    traces = []
                    for enabled in (True, False):
                        result = subprocess.run([str(ADAPTER), str(rom), str(boot)] +
                                                ([] if enabled else ['--end-only']),
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        traces.append(probe.validate_trace(json.loads(result.stdout), case, 'gbb', enabled))
                    self.assertEqual(probe.access.final_state(traces[0]), probe.access.final_state(traces[1]))
                    for ly in (0, 1):
                        edges = probe.line_edges(traces[0]['samples'], ly)
                        for value in edges.values():
                            a, b = value['interval']
                            self.assertLess(a, b)
                            self.assertLessEqual(b - a, 8 if case['speed'] else 16)
                    if case['control'] == 'background' and case['scx'] == case['phase'] == 0:
                        rom.write_bytes(probe.probe_rom(case, 'ldh'))
                        forms = []
                        for enabled in (True, False):
                            result = subprocess.run([str(ADAPTER), str(rom), str(boot)] +
                                                    ([] if enabled else ['--end-only']),
                                                    capture_output=True, text=True, timeout=30)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            forms.append(probe.validate_trace(json.loads(result.stdout), case, 'gbb', enabled, 'ldh'))
                        self.assertEqual(probe.access.final_state(forms[0]), probe.access.final_state(forms[1]))
                        a = {s['clock']: s for s in traces[0]['samples']}
                        b = {s['clock']: s for s in forms[0]['samples']}
                        shared = a.keys() & b.keys()
                        self.assertGreater(len(shared), 1)
                        self.assertTrue(all(a[t][k] == b[t][k] for t in shared for k in ('ly', 'mode', 'frontier')))

    def test_trace_rejects_wrong_clocks_modes_and_completion(self):
        evidence = json.loads(FIXTURE.read_text())
        example = evidence['examples'][0]
        case = next(c for c in probe.scenarios() if c['name'] == example['scenario'])
        trace = example['trace']
        probe.validate_trace(trace, case, trace['core'])
        for key, value in (('clock_unit', 'cpu-t'), ('model', 'cgb0'), ('complete', False),
                           ('qualification', True), ('state_bytes', 0), ('state_fnv1a64', -1)):
            bad = copy.deepcopy(trace)
            bad[key] = value
            with self.assertRaises(ValueError):
                probe.validate_trace(bad, case, trace['core'])
        for key, value in (('clock', 1), ('pc', 0), ('mode', 4), ('frontier', -2)):
            bad = copy.deepcopy(trace)
            bad['samples'][3][key] = value
            with self.assertRaises(ValueError):
                probe.validate_trace(bad, case, trace['core'])
        bad = copy.deepcopy(trace)
        bad['samples'] *= 10
        with self.assertRaises(ValueError):
            probe.validate_trace(bad, case, trace['core'])

    def test_edges_require_complete_monotonic_lines(self):
        example = json.loads(FIXTURE.read_text())['examples'][0]['trace']
        samples = example['samples']
        probe.line_edges(samples, 1)
        for bad in ([], [s for s in samples if s['ly'] != 1], samples[:10]):
            with self.assertRaises(ValueError):
                probe.line_edges(bad, 1)
        bad = copy.deepcopy(samples)
        line = [s for s in bad if s['ly'] == 1]
        line[0]['frontier'] = 160
        with self.assertRaises(ValueError):
            probe.line_edges(bad, 1)
        with self.assertRaises(ValueError):
            probe.interior_deltas(samples, samples[:-1])
        bad = copy.deepcopy(samples)
        bad[0]['clock'] += 1
        with self.assertRaises(ValueError):
            probe.interior_deltas(samples, bad)

    def test_frozen_pairing_and_provenance(self):
        evidence = json.loads(FIXTURE.read_text())
        self.assertFalse(evidence['qualification'])
        self.assertEqual(evidence['reference']['revision'], '208ba4afabffab9edde416f2dbb8ae459e34adb8')
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.access.anchor.owned_boot()).hexdigest())
        for path, sha in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), sha, path)
        cases = {c['name']: c for c in probe.scenarios()}
        rows = evidence['rows']
        self.assertEqual(len(rows), 80)
        self.assertEqual({r['scenario'] for r in rows}, set(cases))
        for row in rows:
            self.assertEqual(row['rom_sha256'], hashlib.sha256(probe.probe_rom(cases[row['scenario']])).hexdigest())
            self.assertEqual(set(row['captures']), {'gbb', 'sameboy'})
            for capture in row['captures'].values():
                self.assertTrue(capture['repeatable'])
                self.assertTrue(capture['snapshot_equivalent'])
                self.assertEqual(len(capture['lines']), 2)
                for line in capture['lines']:
                    self.assertEqual(set(line), {'first_pixel', 'last_pixel', 'mode3_entry', 'mode3_exit'})
                    for edge in line.values():
                        self.assertLess(edge['interval'][0], edge['interval'][1])
            case = cases[row['scenario']]
            if case['control'] == 'background':
                self.assertEqual(row['interior_frontier_deltas'], [[2], [0]] if case['speed'] else [[4], [2]])
        for example in evidence['examples']:
            trace = example['trace']
            case = cases[example['scenario']]
            probe.validate_trace(trace, case, trace['core'])
            row = next(r for r in rows if r['scenario'] == example['scenario'])
            self.assertEqual([probe.line_edges(trace['samples'], ly) for ly in (0, 1)],
                             row['captures'][trace['core']]['lines'])
        controls = evidence['polling_form_controls']
        self.assertEqual(len(controls), 4)
        self.assertEqual({(c['scenario'], c['core']) for c in controls},
                         {(c['name'], core) for c in probe.scenarios()
                          if c['control'] == 'background' and c['scx'] == c['phase'] == 0
                          for core in ('gbb', 'sameboy')})
        for control in controls:
            case = cases[control['scenario']]
            self.assertEqual(control['rom_sha256'], hashlib.sha256(probe.probe_rom(case, 'ldh')).hexdigest())
            self.assertGreater(control['shared_snapshots'], 1)
            self.assertTrue(control['visible_phase_equal'])
            self.assertTrue(control['repeatable'])
            self.assertTrue(control['snapshot_equivalent'])

    def test_retained_reference_object_controls_expose_distinct_fetch_rules(self):
        evidence = json.loads(FIXTURE.read_text())
        rows = {r['scenario']: r for r in evidence['rows']}
        for speed in ('normal', 'double'):
            for scx in (0, 7):
                for phase in (0, 1):
                    for side in ('left', 'middle'):
                        def edges(control, core, ly):
                            return rows[f'{speed}-{control}-scx{scx}-phase{phase}']['captures'][core]['lines'][ly]
                        self.assertEqual(edges('object-' + side, 'sameboy', 0), edges('background', 'sameboy', 0))
                        self.assertNotEqual(edges('object-' + side, 'gbb', 0), edges('background', 'gbb', 0))
                        self.assertEqual(edges('object-disabled-' + side, 'sameboy', 1), edges('object-' + side, 'sameboy', 1))
                        self.assertEqual(edges('object-disabled-' + side, 'gbb', 1), edges('background', 'gbb', 1))
                        self.assertNotEqual(edges('object-disabled-' + side, 'sameboy', 1), edges('background', 'sameboy', 1))

    def test_results_fail_closed_for_missing_cases_or_disagreements(self):
        evidence = json.loads(FIXTURE.read_text())
        self.assertFalse(probe.passes(evidence))
        good = copy.deepcopy(evidence)
        for row in good['rows']:
            row['pixel_match'] = row['mode_match'] = True
        self.assertTrue(probe.passes(good))
        for change in ('missing', 'duplicate', 'pixel', 'mode', 'observer'):
            bad = copy.deepcopy(good)
            if change == 'missing': bad['rows'].pop()
            elif change == 'duplicate': bad['rows'][-1] = bad['rows'][0]
            elif change == 'observer': bad['rows'][0]['captures']['sameboy']['snapshot_equivalent'] = False
            else: bad['rows'][0][change + '_match'] = False
            self.assertFalse(probe.passes(bad))
        bad = copy.deepcopy(good)
        bad['polling_form_controls'].pop()
        self.assertFalse(probe.passes(bad))

    def test_cli_rejects_an_adapter_masquerading_as_the_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/cgb_pixel_phase_probe.py'),
                                     '--gbb-access-trace', str(ADAPTER),
                                     '--sameboy-access-trace', str(ADAPTER),
                                     '--output', str(Path(tmp) / 'out')],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 2)
            self.assertIn('invalid phase trace metadata', result.stderr)


if __name__ == '__main__':
    unittest.main()
