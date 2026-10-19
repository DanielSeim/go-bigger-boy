# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_stat_boundary_read_probe as probe


class StatBoundaryReadContracts(unittest.TestCase):
    @staticmethod
    def memory(scenario, speed=False):
        _, _, count = probe.prefix(scenario, speed)
        data = bytearray(8192)
        data[:2] = b'\x01\x01'
        data[0x3f] = 0xa5
        data[0x10:0x19] = bytes([0, 0, count, 2, 1, 2, 3, 128 if speed else 0, 0x5a])
        return data

    def test_rom_headers_and_same_cost_ime_control(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 288)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], 0xc0)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
            self.assertIn(bytes.fromhex('00 00 af e0 04 c3 00 10'), rom[0x150:0x300])
            self.assertIn(bytes.fromhex('3e 02 ea ff ff'), rom[0x150:0x300])
            self.assertIn(bytes.fromhex('c3 00 11'), rom[0x1000:0x1100])
        for bad in ('original-phase8-lap0-slot0', 'incjr-phase0-lap2-slot0',
                    'incjr-phase0-lap0-slot2'):
            with self.assertRaises(ValueError):
                probe.probe_rom(bad)

    def test_prefix_cycles_and_counter_follow_instruction_semantics(self):
        # Decode each emitted prefix independently of the generator's timing
        # table. JR NZ is taken because D remains 2 and OR A clears Z.
        for scenario in probe.SCENARIOS:
            for speed in (False, True):
                code, predicted, count = probe.prefix(scenario, speed)
                cycles, bc, pc = 0, 0, 0
                while pc < len(code):
                    op = code[pc]
                    pc += 1
                    if op == 0:
                        cycles += 4
                    elif op == 3:
                        bc += 1
                        cycles += 8
                    elif op in (0x7a, 0xb7):
                        cycles += 4
                    else:
                        self.assertIn(op, (0x18, 0x20))
                        self.assertEqual(code[pc], 0)
                        pc += 1
                        cycles += 12
                self.assertEqual((cycles, bc), (predicted, count))
                rom = probe.probe_rom(scenario, speed)
                read = 0x1004 + len(code)
                self.assertEqual(rom[read:read + 2], bytes.fromhex('f0 0f'))
                self.assertEqual(rom[0x1004:read], code)

    def test_incomplete_and_invalid_observations_fail_closed(self):
        scenario = probe.SCENARIOS[0]
        for offset in (0, 1, 0x3f):
            data = self.memory(scenario)
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, scenario, 4)
        for offset, value in ((0x10, 1), (0x11, 1), (0x12, 0), (0x13, 4),
                              (0x14, 2), (0x15, 1), (0x17, 1), (0x18, 0)):
            data = self.memory(scenario)
            data[offset] = value
            self.assertFalse(probe.analyze(data, scenario, 4)['authored_constraints_match'])
        data = self.memory(scenario)
        data[0x10], data[0x15] = 2, 0
        self.assertFalse(probe.analyze(data, scenario, 4)['authored_constraints_match'])
        with self.assertRaises(ValueError):
            probe.analyze(bytes(64), scenario, 4)

    def test_primary_read_difference_is_retained(self):
        scenario = probe.SCENARIOS[0]
        data = self.memory(scenario)
        a = probe.analyze(data, scenario, 4)
        data[0x10] = 2
        b = probe.analyze(data, scenario, 4)
        self.assertTrue(a['authored_constraints_match'] and b['authored_constraints_match'])
        self.assertNotEqual(a['event_sha256'], b['event_sha256'])

    def report(self):
        return dict(qualification=False, rows=[dict(scenario=s, double_speed=speed, frame=4,
            match=True, captures={core: dict(probe.analyze(self.memory(s, speed), s, 4), repeatable=True)
                                  for core in ('gbb', 'sameboy')})
            for s in probe.SCENARIOS for speed in (False, True)])

    def test_complete_grid_both_cores_repeatability_and_speed_required(self):
        report = self.report()
        self.assertTrue(probe.passes(report))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1])))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1] + [report['rows'][0]])))
        for field in ('repeatable', 'authored_constraints_match'):
            changed = copy.deepcopy(report)
            changed['rows'][0]['captures']['gbb'][field] = False
            self.assertFalse(probe.passes(changed))
        changed = copy.deepcopy(report)
        del changed['rows'][0]['captures']['sameboy']
        self.assertFalse(probe.passes(changed))
        changed = copy.deepcopy(report)
        changed['rows'][0]['captures']['gbb']['key1_speed'] = 128
        with tempfile.TemporaryDirectory() as tmp, patch.object(probe.isolated, 'run_probe', return_value=changed):
            self.assertFalse(probe.passes(probe.run_probe(Path('/unused'), Path('/unused'), Path(tmp))))

    def test_public_evidence_sources_and_raw_observation_schema(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-stat-boundary-read.json').read_text())
        self.assertFalse(evidence['qualification'])
        for source, digest in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / source).read_bytes()).hexdigest(), digest)
        rows = evidence['rows']
        self.assertEqual(len(rows), 288)
        self.assertEqual({(r['scenario'], r['double_speed']) for r in rows},
                         {(s, speed) for s in probe.SCENARIOS for speed in (False, True)})
        for row in rows:
            self.assertEqual(row['rom_sha256'], hashlib.sha256(
                probe.probe_rom(row['scenario'], row['double_speed'])).hexdigest())
            self.assertEqual(row['match'], row['gbb'] == row['sameboy'])
            for core in ('gbb', 'sameboy'):
                data = self.memory(row['scenario'], row['double_speed'])
                data[0x10:0x19] = bytes(row[core])
                observed = probe.analyze(data, row['scenario'], row['frame'])
                self.assertTrue(observed['authored_constraints_match'])
                self.assertEqual(observed['key1_speed'], 128 if row['double_speed'] else 0)
        self.assertEqual(sum(r['match'] for r in rows), evidence['summary']['matches'])

    def test_cli_preserves_mismatch_and_capture_failure(self):
        report = self.report()
        report['rows'][0]['match'] = False
        argv = ['probe', '--runner', '/unused', '--sameboy-capture', '/unused', '--output', '/unused']
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        with patch.object(probe, 'run_probe', side_effect=ValueError('incomplete')), \
                patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as error:
            probe.main()
        self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
