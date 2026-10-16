#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""First-vector clock witnesses and optional native paired capture contracts."""
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_irq_div_probe as probe


class Contracts(unittest.TestCase):
    def test_roms_and_first_vector_instruction(self):
        for speed in (False, True):
            for scenario in probe.PHASE_SCENARIOS:
                source, phase, read = probe.split(scenario)
                rom = probe.probe_rom(scenario, speed)
                self.assertEqual(len(rom), 32768)
                self.assertEqual(rom[0x50:0x50 + read + 2], bytes(read) + bytes.fromhex('f0 04'))
                checksum = 0
                for value in rom[0x134:0x14d]:
                    checksum = (checksum - value - 1) & 255
                self.assertEqual(rom[0x14d], checksum)
                self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                                 sum(rom[:0x14e] + rom[0x150:]) & 65535)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    def test_div_boundary_and_request_alignment(self):
        self.assertEqual(probe.times(43, 0), (212, 244))
        self.assertEqual(probe.times(44, 0), (212, 244))
        self.assertEqual(probe.times(44, 3), (212, 256))
        self.assertEqual(probe.times(48, 0), (228, 260))
        for phase in (43, 44, 47, 48, 51, 52):
            request, _ = probe.times(phase, 0)
            self.assertEqual(4 * ((request - 20) // 4) + 20, request)

    def test_software_request_phase_sweep(self):
        self.assertEqual(len(probe.SCENARIOS), 48)
        self.assertEqual(len(probe.PHASE_SCENARIOS), 96)
        for source, offset in probe.SOFTWARE_OFFSETS.items():
            case = f'{source}-start-44-read-3'
            data = self.memory()
            data[0x100] = 0 if offset < 0 else 1
            result = probe.analyze(data, case, 2)
            self.assertEqual(result['software_request_offset_t_cycles'], offset)
            self.assertEqual(result['predicted_request_t_cycles'], 212 + offset)
            self.assertEqual(result['predicted_div_read_t_cycles'], 256 + offset)
            self.assertTrue(result['div_prediction_match'])
            rom = probe.probe_rom(case)
            reset = rom.index(bytes.fromhex('fb 00 af e0 04'), 0x150) + 5
            assertion = rom.index(bytes.fromhex('3e 04 e0 0f'), reset)
            self.assertEqual((assertion - reset) * 4 + 20, 212 + offset)
        capture = dict(repeatable=True, authored_constraints_match=True)
        rows = [dict(scenario=scenario, double_speed=speed, match=True,
                     captures=dict(gbb=capture, sameboy=capture))
                for scenario in probe.PHASE_SCENARIOS for speed in (False, True)]
        report = dict(request_phase_sweep=True, rows=rows,
                      request_relations=[dict(match=True) for _ in range(288)])
        self.assertTrue(probe.passes(report))
        report['request_relations'][0]['match'] = False
        self.assertFalse(probe.passes(report))
        report['request_relations'][0]['software_request_offset_t_cycles'] = -4
        self.assertTrue(probe.passes(report))
        report['request_relations'][0]['match'] = True
        report['rows'] = rows[:-1]
        self.assertFalse(probe.passes(report))

    @staticmethod
    def memory():
        data = bytearray(8192)
        data[:2] = bytes((1, 1))
        data[0x100:0x103] = bytes((1, 255, 0xe0))
        data[0x200] = 0xa5
        return data

    def test_prediction_is_diagnostic_and_if_is_a_witness(self):
        data = self.memory()
        result = probe.analyze(data, 'software-start-44-read-3', 2)
        self.assertTrue(result['div_prediction_match'])
        data[0x100] = 0
        result = probe.analyze(data, 'software-start-44-read-3', 2)
        self.assertFalse(result['div_prediction_match'])
        self.assertTrue(result['record_structure_match'])
        data[0x102] |= 4
        self.assertFalse(probe.analyze(data, 'software-start-44-read-3', 2)['record_structure_match'])

    def test_incomplete_logs_rejected(self):
        for offset in (0, 1, 0x200):
            data = self.memory()
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'timer-start-44-read-3', 2)

    def test_missing_or_different_results_fail(self):
        self.assertFalse(probe.passes(dict(rows=[], request_relations=[])))
        capture = dict(repeatable=True, authored_constraints_match=True)
        rows = [dict(scenario=scenario, double_speed=speed, match=True,
                     captures=dict(gbb=capture, sameboy=capture))
                for scenario in probe.SCENARIOS for speed in (False, True)]
        report = dict(rows=rows, request_relations=[dict(match=True) for _ in range(96)])
        self.assertTrue(probe.passes(report))
        rows[0]['match'] = False
        self.assertFalse(probe.passes(report))
        rows[0]['match'] = True
        report['request_relations'][0]['match'] = False
        self.assertFalse(probe.passes(report))
        report['request_relations'][0]['match'] = True
        report['rows'] = rows[:-1]
        self.assertFalse(probe.passes(report))


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set paired native capture tools')
class NativeContracts(unittest.TestCase):
    def test_repeatable_complete_logs_and_truthful_comparison(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                    Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                    Path(tmp) / 'results', request_phase_sweep=True)
            self.assertEqual(len(report['rows']), 192)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][e]['events'] for e in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                for c in row['captures'].values():
                    self.assertTrue(c['repeatable'])
                    self.assertTrue(c['record_structure_match'])
                    self.assertEqual(c['div_prediction_match'], c['events'][0][0] == c['expected_div'])
            self.assertEqual(len(report['request_relations']), 288)
            for relation in report['request_relations']:
                self.assertEqual(relation['match'], relation['timer'] == relation['software'])
            for scenario in probe.PHASE_SCENARIOS:
                rows = [r for r in report['rows'] if r['scenario'] == scenario]
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(rows[0]['captures'][name]['events'], rows[1]['captures'][name]['events'])



if __name__ == '__main__':
    unittest.main()
