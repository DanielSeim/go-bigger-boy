# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_lcd_bus_phase_probe as probe


class LcdBusPhaseContracts(unittest.TestCase):
    def test_authored_forms_have_valid_headers_and_distinct_programs(self):
        roms = [probe.write_rom(f, s) for f in probe.WRITES for s in (False, True)]
        roms += [probe.read_rom(f, s) for f in probe.SCENARIOS for s in (False, True)]
        self.assertEqual(len(set(roms)), 86)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        for bad in ('bad', 'ldh-phase-13'):
            with self.assertRaises(ValueError): probe.read_rom(bad)
        with self.assertRaises(ValueError): probe.write_rom('bad')

    def test_equal_cost_reads_align_the_memory_cycle(self):
        # Decode independently: NOP=4, LDH opcode/immediate=8 before read,
        # absolute opcode/two immediates=12, LD A,(C) opcode=4 before read.
        for form, code in probe.READS.items():
            cycles, pc = 0, 0
            while code[pc] == 0:
                cycles += 4
                pc += 1
            opcode = code[pc]
            cycles += {0xf0: 8, 0xfa: 12, 0xf2: 4}[opcode]
            self.assertEqual(cycles, 12)
            for speed in (False, True):
                rom = probe.read_rom(f'{form}-phase-0', speed)
                start = 0x1000 + 2 + 4 + 108*(2 if speed else 1)
                self.assertEqual(rom[0x1000:0x1002], bytes.fromhex('0e 0f'))
                self.assertEqual(rom[start:start+len(code)], code)
                self.assertEqual(rom[start+len(code):start+len(code)+3], bytes.fromhex('e6 03 22'))

    def report(self):
        return dict(writes=[dict(form=f, double_speed=s, match=True, lcd_phase_match=False,
                    captures={c: dict(repeatable=True, observer_equivalent=True) for c in ('gbb','sameboy')})
                    for f in probe.WRITES for s in (False,True)],
            reads=[dict(scenario=f, double_speed=s, frame=4, match=True,
                    captures={c: dict(repeatable=True, authored_constraints_match=True) for c in ('gbb','sameboy')})
                    for f in probe.SCENARIOS for s in (False,True)],
            read_form_relations=[dict(double_speed=s, phase=n, core=c, match=True)
                    for s in (False,True) for n in range(13) for c in ('gbb','sameboy')])

    def test_missing_duplicate_or_unrepeatable_cases_fail_closed(self):
        report = self.report()
        self.assertTrue(probe.passes(report))
        for key in ('writes','reads','read_form_relations'):
            changed = copy.deepcopy(report)
            changed[key].pop()
            self.assertFalse(probe.passes(changed))
            changed = copy.deepcopy(report)
            changed[key][-1] = changed[key][0]
            self.assertFalse(probe.passes(changed))
        for key in ('writes','reads'):
            changed = copy.deepcopy(report)
            changed[key][0]['double_speed'] = 0
            self.assertFalse(probe.passes(changed))
            changed = copy.deepcopy(report)
            changed[key][0]['captures']['gbb']['repeatable'] = False
            self.assertFalse(probe.passes(changed))
        changed = copy.deepcopy(report)
        changed['writes'][0]['captures']['gbb']['observer_equivalent'] = False
        self.assertFalse(probe.passes(changed))

    def test_bad_memory_and_mismatched_primary_reads_are_not_masked(self):
        with self.assertRaises(ValueError): probe.analyze(bytes(8192),'ldh-phase-0',4)
        memory = bytearray(8192)
        memory[:2] = b'\x01\x01'; memory[0x3f] = 0xa5
        memory[0x10:0x16] = bytes([0,2,2,1,3,0])
        a = probe.analyze(memory,'ldh-phase-0',4)
        memory[0x10] = 2
        b = probe.analyze(memory,'absolute-phase-0',4)
        self.assertTrue(a['authored_constraints_match'] and b['authored_constraints_match'])
        self.assertNotEqual(a['event_sha256'], b['event_sha256'])

    def test_evidence_matches_producer_and_authored_grid(self):
        evidence = json.loads((ROOT/'tests/fixtures/external-reference/cgb-lcd-bus-phase.json').read_text())
        self.assertFalse(evidence['qualification'])
        self.assertFalse(evidence['candidate_accepted'])
        self.assertEqual(hashlib.sha256((ROOT/evidence['candidate_patch']).read_bytes()).hexdigest(),
                         evidence['candidate_patch_sha256'])
        for variant in evidence['ablation_variants'].values():
            self.assertFalse(variant['accepted'])
            self.assertEqual(hashlib.sha256((ROOT/variant['patch']).read_bytes()).hexdigest(),
                             variant['patch_sha256'])
        for source,digest in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT/source).read_bytes()).hexdigest(),digest)
        for speed in (False,True):
            for form in probe.WRITES:
                row = next(r for r in evidence['writes'] if (r['form'],r['double_speed']) == (form,speed))
                self.assertEqual(row['rom_sha256'],hashlib.sha256(probe.write_rom(form,speed)).hexdigest())
            for scenario in probe.SCENARIOS:
                row = next(r for r in evidence['reads'] if (r['scenario'],r['double_speed']) == (scenario,speed))
                self.assertEqual(row['rom_sha256'],hashlib.sha256(probe.read_rom(scenario,speed)).hexdigest())
        self.assertEqual(len(evidence['writes']),8)
        self.assertEqual(len(evidence['reads']),78)

    def test_cli_retains_mismatch_and_error_exit_codes(self):
        report = self.report(); report['writes'][0]['match'] = False
        argv=['probe','--gbb-clock-trace','/unused','--sameboy-clock-trace','/unused',
              '--runner','/unused','--sameboy-capture','/unused','--output','/unused']
        with patch.object(probe,'run_probe',return_value=report),patch.object(sys,'argv',argv),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(),1)
        with patch.object(probe,'run_probe',side_effect=ValueError('bad capture')),patch.object(sys,'argv',argv),contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as error:
            probe.main()
        self.assertEqual(error.exception.code,2)


if __name__ == '__main__': unittest.main()
