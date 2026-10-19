# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_vram_tail_probe as probe

ADAPTER = Path(sys.argv.pop(1)).resolve()
FIXTURE = ROOT / 'tests/fixtures/external-reference/cgb-vram-tail.json'


class VramTailContracts(unittest.TestCase):
    def test_owned_rom_grid_and_checksums(self):
        cases = probe.scenarios()
        self.assertEqual(len(cases), 496)
        roms = [probe.probe_rom(c) for c in cases]
        self.assertEqual(len(set(roms)), 496)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], 0xc0)
            self.assertEqual(rom[0x147], 0)
            check = 0
            for value in rom[0x134:0x14d]:
                check = (check - value - 1) & 255
            self.assertEqual(rom[0x14d], check)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'), sum(rom[:0x14e] + rom[0x150:]) & 65535)

    def test_live_bank_address_and_row_controls_preserve_serialized_state(self):
        evidence = json.loads(FIXTURE.read_text())
        expected = {r['scenario']: r['captures']['gbb']['value'] for r in evidence['rows']}
        cases = [c for c in probe.scenarios() if c['dot'] == 264 and c['sentinel'] == 0xa5 and
                 c['palette'] == 0 and (c['low'], c['high']) in ((0, 0), (0xaa, 0x55)) and
                 (not c.get('row_ramp') or c['scy'] == 3)]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot, rom = root / 'boot.bin', root / 'probe.gbc'
            boot.write_bytes(probe.objects.phase.access.anchor.owned_boot())
            for case in cases:
                with self.subTest(case=case['name']):
                    rom.write_bytes(probe.probe_rom(case))
                    traces = []
                    for enabled in (True, False):
                        result = subprocess.run([str(ADAPTER), str(rom), str(boot)] +
                                                ([] if enabled else ['--end-only']),
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        traces.append(probe.objects.validate_trace(json.loads(result.stdout), probe.schedule(case), 'gbb', enabled))
                    self.assertEqual(probe.objects.phase.access.final_state(traces[0]),
                                     probe.objects.phase.access.final_state(traces[1]))
                    self.assertEqual(probe.objects.value(traces[0], probe.schedule(case)), expected[case['name']])

    def test_frozen_reference_bank_row_and_prefetch_correlations(self):
        evidence = json.loads(FIXTURE.read_text())
        self.assertFalse(evidence['qualification'])
        self.assertEqual(evidence['reference']['revision'], '208ba4afabffab9edde416f2dbb8ae459e34adb8')
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.objects.phase.access.anchor.owned_boot()).hexdigest())
        for path, sha in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), sha, path)
        cases = {c['name']: c for c in probe.scenarios()}
        rows = evidence['rows']
        self.assertEqual(len(rows), 496)
        self.assertEqual({r['scenario'] for r in rows}, set(cases))
        for row in rows:
            c = cases[row['scenario']]
            self.assertEqual(row['rom_sha256'], hashlib.sha256(probe.probe_rom(c)).hexdigest())
            expected = 255 if c['dot'] < 264 or (c['speed'] and c['dot'] == 264) else c['sentinel']
            if not c['speed'] and c['dot'] == 264:
                if c.get('row_ramp'):
                    if c['last_tile'] == 2:
                        expected = 0xd2 if c['cpu_bank'] == c['bg_bank'] else 0xe3
                    else:
                        y = (c['scy'] + 1) & 7
                        expected = (y + 1)*0x11 if c['cpu_bank'] == c['bg_bank'] else 0x81 + y
                else:
                    expected = c['high'] if c['cpu_bank'] == c['bg_bank'] else c['other_high']
            self.assertEqual(row['captures']['sameboy']['value'], expected, row['scenario'])
            for capture in row['captures'].values():
                self.assertTrue(capture['repeatable'])
                self.assertTrue(capture['snapshot_equivalent'])
                self.assertTrue(capture['timing_control_equal'])
                self.assertEqual(capture['classification'], probe.classification(capture['value'], c))
            self.assertEqual(row['match'], row['captures']['gbb']['value'] == expected)
        groups = evidence['timing_controls']
        self.assertEqual(len(groups), 16)
        self.assertEqual(sum(g['cases'] for g in groups), 992)
        for example in evidence['examples']:
            trace = example['trace']
            c = cases[example['scenario']]
            probe.objects.validate_trace(trace, probe.schedule(c), trace['core'])
            group = next(g for g in groups if g['core'] == trace['core'] and
                         g['speed'] == c['speed'] and g['dot'] == c['dot'])
            digest = hashlib.sha256(json.dumps(probe.timing(trace), separators=(',', ':')).encode()).hexdigest()
            self.assertEqual(digest, group['sha256'])

    def test_data_variation_with_changed_timing_is_rejected(self):
        evidence = json.loads(FIXTURE.read_text())
        example = evidence['examples'][0]
        first = next(c for c in probe.scenarios() if c['name'] == example['scenario'])
        second = next(c for c in probe.scenarios() if c['speed'] == first['speed'] and
                      c['dot'] == first['dot'] and c['name'] != first['name'])
        traces = {e['trace']['core']: e['trace'] for e in evidence['examples'] if e['scenario'] == first['name']}

        def run(args, **_):
            trace = copy.deepcopy(traces[args[0]])
            if second['name'] in args[1]:
                trace['samples'][4]['mode'] ^= 1
            if '--end-only' in args:
                trace['samples'] = []
                trace['snapshots_enabled'] = False
            return SimpleNamespace(returncode=0, stdout=json.dumps(trace), stderr='')

        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(probe, 'scenarios', return_value=[first, second]), patch.object(probe.subprocess, 'run', side_effect=run):
                with self.assertRaisesRegex(ValueError, 'variation changed visible transfer timing'):
                    probe.run_probe('gbb', 'sameboy', Path(tmp) / 'out')
            self.assertFalse((Path(tmp) / 'out/report.json').exists())

    def test_unknown_read_values_are_data_and_disagreements_fail_closed(self):
        case = probe.scenarios()[0]
        self.assertEqual(probe.classification(0, case), 'other')
        self.assertEqual(probe.classification(255, case), 'ff')
        report = json.loads(FIXTURE.read_text())
        self.assertFalse(probe.passes(report))
        good = copy.deepcopy(report)
        for row in good['rows']:
            row['match'] = True
            row['captures']['gbb']['value'] = row['captures']['sameboy']['value']
        self.assertTrue(probe.passes(good))
        for action in ('missing', 'duplicate', 'timing', 'observer', 'value'):
            bad = copy.deepcopy(good)
            if action == 'missing': bad['rows'].pop()
            elif action == 'duplicate': bad['rows'][-1] = bad['rows'][0]
            elif action == 'value': bad['rows'][0]['captures']['gbb']['value'] ^= 1
            else: bad['rows'][0]['captures']['sameboy']['timing_control_equal' if action == 'timing' else 'snapshot_equivalent'] = False
            self.assertFalse(probe.passes(bad))


if __name__ == '__main__':
    unittest.main()
