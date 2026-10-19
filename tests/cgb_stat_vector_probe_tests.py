# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import copy
import io
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_stat_vector_probe as probe


class StatVectorContracts(unittest.TestCase):
    @staticmethod
    def memory(scenario, speed=False):
        data = bytearray(8192)
        data[:2] = b'\x01\x01'
        data[0x3f] = 0xa5
        data[0x10:0x1a] = bytes([0, 0, 4, 16, 3, 0, 0, 0, 128 if speed else 0, 0x5a])
        return data

    def test_rom_headers_and_terminal_handler(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 64)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
            self.assertEqual(rom[0x48:0x4c], bytes.fromhex('78 c3 00 03'))
            self.assertEqual(rom[0x300:0x308], bytes.fromhex('22 79 22 d1 7b 22 7a 22'))
            self.assertNotIn(bytes.fromhex('c3 00 11'), rom[0x1000:0x1100])
        for bad in ('stat-incjr-phase8', 'stat-unknown-phase0'):
            with self.assertRaises(ValueError):
                probe.probe_rom(bad)

    def test_post_trigger_alignment_and_real_instruction_boundaries(self):
        for source in ('stat', 'software'):
            for pattern in probe.PATTERNS:
                for phase in range(8):
                    scenario = f'{source}-{pattern}-phase{phase}'
                    code = probe.window(scenario)
                    self.assertEqual(code[4:4 + phase], bytes(phase))
                    self.assertEqual(code[4 + phase:], probe.PATTERNS[pattern])
                    boundaries = probe.return_boundaries(scenario)
                    self.assertIn(0x1004, boundaries)
                    # The JR displacement byte is never an instruction boundary.
                    self.assertNotIn(0x1000 + len(code) - 1, boundaries)
        software = probe.probe_rom('software-incjr-phase0')
        self.assertIn(bytes.fromhex('3e 00 e0 41'), software[0x150:0x300])
        self.assertEqual(software[0x1000:0x1004], bytes.fromhex('3e 02 e0 0f'))

    def test_missing_markers_and_invalid_witnesses_fail_closed(self):
        scenario = 'software-incjr-phase0'
        for offset in (0, 1, 0x3f):
            data = self.memory(scenario)
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, scenario, 4)
        for offset, value in ((0x10, 1), (0x11, 1), (0x12, 6), (0x15, 1),
                              (0x16, 1), (0x17, 2), (0x18, 1), (0x19, 0)):
            data = self.memory(scenario)
            data[offset] = value
            self.assertFalse(probe.analyze(data, scenario, 4)['authored_constraints_match'])
        with self.assertRaises(ValueError):
            probe.analyze(bytes(64), scenario, 4)

    def test_counter_and_stacked_pc_differences_remain_visible(self):
        scenario = 'stat-original-phase0'
        data = self.memory(scenario)
        before = probe.analyze(data, scenario, 4)
        for offset, value in ((0x11, 17), (0x12, 5)):
            changed = bytearray(data)
            changed[offset] = value
            after = probe.analyze(changed, scenario, 4)
            self.assertTrue(after['authored_constraints_match'])
            self.assertNotEqual(before['event_sha256'], after['event_sha256'])

    def report(self):
        return dict(qualification=False, rows=[dict(scenario=s, double_speed=speed, frame=4,
            match=True, captures={core: dict(probe.analyze(self.memory(s, speed), s, 4), repeatable=True)
                                  for core in ('gbb', 'sameboy')})
            for s in probe.SCENARIOS for speed in (False, True)])

    def test_complete_grid_repeatability_and_speed_required(self):
        report = self.report()
        self.assertTrue(probe.passes(report))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1])))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1] + [report['rows'][0]])))
        for field in ('repeatable', 'authored_constraints_match'):
            changed = copy.deepcopy(report)
            changed['rows'][0]['captures']['gbb'][field] = False
            self.assertFalse(probe.passes(changed))
        changed = copy.deepcopy(report)
        changed['rows'][0]['captures']['gbb']['key1_speed'] = 128
        with tempfile.TemporaryDirectory() as tmp, patch.object(probe.isolated, 'run_probe', return_value=changed):
            self.assertFalse(probe.passes(probe.run_probe(Path('/unused'), Path('/unused'), Path(tmp))))

    def test_recorded_evidence_matches_authored_sources_and_witnesses(self):
        evidence = json.loads((ROOT / 'tests/fixtures/external-reference/cgb-stat-vector.json').read_text())
        self.assertFalse(evidence['qualification'])
        self.assertEqual(evidence['probe_sha256'], hashlib.sha256(
            (ROOT / 'scripts/cgb_stat_vector_probe.py').read_bytes()).hexdigest())
        rows = evidence['rows']
        self.assertEqual(len(rows), 64)
        self.assertEqual({(r['scenario'], r['double_speed']) for r in rows},
                         {(s, speed) for s in probe.SCENARIOS for speed in (False, True)})
        for row in rows:
            self.assertEqual(row['rom_sha256'], hashlib.sha256(
                probe.probe_rom(row['scenario'], row['double_speed'])).hexdigest())
            self.assertEqual(row['match'], row['gbb'] == row['sameboy'])
            for core in ('gbb', 'sameboy'):
                data = self.memory(row['scenario'], row['double_speed'])
                data[0x10:0x1a] = bytes(row[core])
                observed = probe.analyze(data, row['scenario'], row['frame'])
                self.assertTrue(observed['authored_constraints_match'])
                self.assertEqual(observed['key1_speed'], 128 if row['double_speed'] else 0)
        self.assertEqual(sum(r['match'] for r in rows), evidence['summary']['matches'])

    def test_cli_keeps_reference_mismatch_failure(self):
        report = self.report()
        report['rows'][0]['match'] = False
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused', '--sameboy-capture', '/unused',
                                           '--output', '/unused']), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


if __name__ == '__main__':
    unittest.main()
