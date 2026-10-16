#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Software interrupt order, timing controls and truthful comparison contracts."""
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_reti_dispatch_probe as probe


class ProbeContracts(unittest.TestCase):
    def test_rom_headers_and_all_phase_controls(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 48)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown-phase-0')

    @staticmethod
    def memory(events):
        data = bytearray(8192)
        data[:2] = bytes((len(events), 1))
        data[0x200] = 0xa5
        for i, event in enumerate(events):
            data[0x100 + 4 * i:0x104 + 4 * i] = bytes(event)
        return data

    def test_pending_order_and_main_marker(self):
        events = [[2, 7, 4, 0], [4, 17, 0, 0]]
        self.assertTrue(probe.analyze(self.memory(events), 'pending-reti-phase-0', 2)['authored_constraints_match'])
        events[1][3] = 1
        self.assertFalse(probe.analyze(self.memory(events), 'pending-reti-phase-0', 2)['authored_constraints_match'])
        self.assertTrue(probe.analyze(self.memory(events), 'pending-ret-phase-0', 2)['authored_constraints_match'])
        self.assertFalse(probe.analyze(self.memory(events[::-1]), 'pending-ret-phase-0', 2)['authored_constraints_match'])

    def test_raised_request_and_single_entry(self):
        events = [[2, 7, 0, 0], [4, 17, 0, 0]]
        self.assertTrue(probe.analyze(self.memory(events), 'raised-reti-phase-0', 2)['authored_constraints_match'])
        events[0][2] = 4
        self.assertFalse(probe.analyze(self.memory(events), 'raised-reti-phase-0', 2)['authored_constraints_match'])
        for kind in ('straight-call', 'timer-only'):
            self.assertTrue(probe.analyze(self.memory([[4, 7, 0, 0]]), kind + '-phase-0', 2)['authored_constraints_match'])

    def test_completion_guard_and_count(self):
        for offset in (0, 1, 0x200):
            data = self.memory([[4, 7, 0, 0]])
            data[offset] = 0
            with self.assertRaises(ValueError):
                probe.analyze(data, 'timer-only-phase-0', 2)

    @staticmethod
    def relation_report():
        rows = []
        for speed in (False, True):
            for phase in range(4):
                for kind in probe.KINDS:
                    value = 10 if kind in ('straight-call', 'timer-only') else 20
                    value += 2 if kind == 'pending-ret' else int(phase == 3) if kind == 'pending-ei-ret' else 0
                    capture = dict(events=[[4, value, 0, 0]])
                    rows.append(dict(scenario=f'{kind}-phase-{phase}', double_speed=speed,
                                     captures=dict(gbb=capture.copy(), sameboy=capture.copy())))
        return dict(rows=rows)

    def test_instruction_timing_relations_reject_wrong_delay(self):
        report = self.relation_report()
        self.assertTrue(all(r['authored_timing_relations_match'] for r in probe.relations(report)))
        row = next(r for r in report['rows'] if r['scenario'] == 'pending-ret-phase-0')
        row['captures']['gbb'] = dict(events=[[4, 23, 0, 0]])
        self.assertFalse(probe.relations(report)[0]['authored_timing_relations_match'])

    def test_cli_does_not_accept_timer_phase_mismatch(self):
        capture = dict(repeatable=True, authored_constraints_match=True)
        report = dict(qualification=False, rows=[dict(match=False, captures=dict(gbb=capture, sameboy=capture))],
                      timing_relations=[dict(authored_timing_relations_match=True)] * 4)
        with patch.object(probe, 'run_probe', return_value=report), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        report['rows'][0]['match'] = True
        report['timing_relations'] = []
        self.assertFalse(probe.passes(report))


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set GBB_TEST_RUNNER and GBB_SAMEBOY_CGB_CAPTURE for native dispatch measurements')
class NativeProbeContracts(unittest.TestCase):
    def test_dispatch_semantics_and_phase_reporting(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_probe(Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                                     Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(),
                                     Path(tmp) / 'results')
            self.assertEqual(len(report['rows']), 48)
            self.assertFalse(report['qualification'])
            self.assertTrue(probe.passes(report))
            for row in report['rows']:
                a, b = (row['captures'][name]['events'] for name in ('gbb', 'sameboy'))
                self.assertEqual(row['different_event_indices'], [i for i in range(len(a)) if a[i] != b[i]])
                self.assertEqual(row['match'], a == b)
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertTrue(capture['authored_constraints_match'])
            for relation in report['timing_relations']:
                self.assertEqual(relation['ret_minus_reti_ticks'], [2] * 4)
                self.assertEqual(sorted(relation['ei_ret_minus_reti_ticks']), [0, 0, 0, 1])
            # Software-only stopwatch logs should be independent of CPU speed.
            for scenario in probe.SCENARIOS:
                rows = [r for r in report['rows'] if r['scenario'] == scenario]
                for name in ('gbb', 'sameboy'):
                    self.assertEqual(rows[0]['captures'][name]['events'], rows[1]['captures'][name]['events'])


if __name__ == '__main__':
    unittest.main()
