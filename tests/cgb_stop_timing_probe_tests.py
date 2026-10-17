#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded instruction-window clocks, malformed witness rejection and native controls."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_stop_timing_probe as probe


class Contracts(unittest.TestCase):
    def test_roms_headers_and_window_instruction_boundaries(self):
        roms = []
        for speed in (False, True):
            for scenario in probe.SCENARIOS:
                rom = probe.probe_rom(scenario, speed)
                roms.append(rom)
                self.assertEqual(len(rom), 32768)
                self.assertEqual(rom[0x143], 0xc0)
                self.assertEqual(rom[probe.START_PC], 0x10 if probe.split(scenario)[1] == 'switch' else 0)
                self.assertEqual(rom[probe.resume_pc(scenario):probe.end_pc(scenario)], probe.resume_instruction(scenario))
                self.assertEqual(rom[probe.end_pc(scenario):probe.end_pc(scenario) + 2], b'\x00\x01')
                checksum = 0
                for value in rom[0x134:0x14d]:
                    checksum = (checksum - value - 1) & 255
                self.assertEqual(rom[0x14d], checksum)
                self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                                 sum(rom[:0x14e] + rom[0x150:]) & 65535)
        self.assertEqual(len(set(roms)), 20)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    @staticmethod
    def gbb_result():
        hit = 'CPU_FIRST_PC cycle=100 normalized_8mhz=200 duration=4 pc=0600 opcode=c3 next_pc=1000 bc=1000 transition=instruction\n'
        stack = 'CPU_STACK_DIAGNOSTICS cycle=96 pc=0600 next_pc=1000 double_speed=false\n'
        return subprocess.CompletedProcess([], 0, 'PASS (Mooneye)\n', hit + stack)

    def test_gbb_clock_requires_protocol_and_unique_speed_witness(self):
        result = self.gbb_result()
        actual = probe.first_pc(result, probe.START_PC)
        self.assertEqual(actual['cpu_t_cycles'], 100)
        self.assertEqual(actual['normalized_8mhz_clocks'], 200)
        self.assertEqual(actual['bc'], 0x1000)
        self.assertFalse(actual['double_speed'])
        for changed in (subprocess.CompletedProcess([], 1, result.stdout, result.stderr),
                        subprocess.CompletedProcess([], 0, 'TIMEOUT', result.stderr),
                        subprocess.CompletedProcess([], 0, result.stdout, result.stderr.splitlines()[0]),
                        subprocess.CompletedProcess([], 0, result.stdout, result.stderr * 2)):
            with self.assertRaises(ValueError):
                probe.first_pc(changed, probe.START_PC)
        with self.assertRaises(ValueError):
            probe.first_pc(result, 0x1001)

    @staticmethod
    def reference_data():
        data = dict(schema='gbb-sameboy-pc-clock-window-v1', qualification=False, model='cgb-e',
                    result='complete', anchor='gb_run_call_before_instruction_callback',
                    start_pc=0x1000, end_pc=0x1003, entry_seen=True, exit_seen=True,
                    entry_opcode=0x10, exit_opcode=0, entry_8mhz_clocks=100, exit_8mhz_clocks=300,
                    elapsed_8mhz_clocks=200, entry_key1=0x7f, exit_key1=0xfe,
                    entry_div=0, exit_div=0, entry_tima=0x30, exit_tima=0xb0,
                    entry_pcm12=0, exit_pcm12=0, entry_bc=0x1000, exit_bc=0x1000,
                    instruction_callbacks=2, callback_trace_truncated=False)
        data['callback_trace'] = [dict(pc=0x1000, opcode=0x10, clocks_8mhz=100, bc=0x1000),
                                  dict(pc=0x1003, opcode=0, clocks_8mhz=300, bc=0x1000)]
        return data

    def test_reference_rejects_incomplete_corrupt_and_unbounded_windows(self):
        data = self.reference_data()
        result = subprocess.CompletedProcess([], 0, json.dumps(data), '')
        self.assertEqual(probe.reference_window(result, 'active-switch', False), data)
        variants = [[], dict(data, result='timeout'), dict(data, entry_seen=1), dict(data, qualification=0),
                    dict(data, elapsed_8mhz_clocks=201), dict(data, elapsed_8mhz_clocks=True),
                    dict(data, callback_trace_truncated=True), dict(data, callback_trace=data['callback_trace'] * 5),
                    dict(data, instruction_callbacks=1), dict(data, exit_key1=0xff),
                    dict(data, entry_bc=65536), dict(data, end_pc=4099.0)]
        corrupt = copy.deepcopy(data)
        corrupt['callback_trace'][-1]['clocks_8mhz'] = 99
        variants.append(corrupt)
        for changed in variants:
            with self.assertRaises(ValueError):
                probe.reference_window(subprocess.CompletedProcess([], 0, json.dumps(changed), ''), 'active-switch', False)
        with self.assertRaises(ValueError):
            probe.reference_window(subprocess.CompletedProcess([], 3, json.dumps(data), ''), 'active-switch', False)

    def test_incomplete_duplicate_or_architecturally_wrong_comparison_fails(self):
        capture = dict(repeatable=True)
        rows = [dict(scenario=s, initial_double_speed=speed, match=True, architectural_match=True,
                     captures=dict(gbb=dict(capture), sameboy=dict(capture)))
                for s in probe.SCENARIOS for speed in (False, True)]
        self.assertTrue(probe.passes(dict(rows=rows)))
        self.assertFalse(probe.passes(dict(rows=rows[:-1])))
        self.assertFalse(probe.passes(dict(rows=[rows[0]] * len(rows))))
        for key in ('match', 'architectural_match'):
            rows[0][key] = False
            self.assertFalse(probe.passes(dict(rows=rows)))
            rows[0][key] = True
        rows[0]['captures']['sameboy']['repeatable'] = False
        self.assertFalse(probe.passes(dict(rows=rows)))
        rows[0]['captures'].pop('sameboy')
        self.assertFalse(probe.passes(dict(rows=rows)))

    def test_cli_disagreement_is_not_success(self):
        with patch.object(probe, 'run_probe', return_value=dict(rows=[])), \
                patch.object(sys, 'argv', ['probe', '--runner', '/unused', '--sameboy-capture', '/unused',
                    '--output', '/unused']), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


@unittest.skipUnless(os.environ.get('GBB_TEST_RUNNER') and os.environ.get('GBB_SAMEBOY_CGB_CAPTURE'),
                     'set native runner and rebuilt pinned adapter with --pc-clock-window support')
class NativeContracts(unittest.TestCase):
    def test_windows_calibrate_resume_cadence_and_register_effects(self):
        runner = Path(os.environ['GBB_TEST_RUNNER']).resolve()
        adapter = Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = probe.run_probe(runner, adapter, root / 'results')
            self.assertEqual(len(report['rows']), 20)
            self.assertFalse(report['qualification'])
            for row in report['rows']:
                a, b = (row['captures'][n] for n in ('gbb', 'sameboy'))
                self.assertTrue(a['repeatable'])
                self.assertTrue(b['repeatable'])
                self.assertTrue(row['architectural_match'])
                self.assertEqual(a['entry']['bc'], 0x1000)
                self.assertEqual(a['exit']['bc'], 0x1100 if row['scenario'].endswith(('-inc', '-load')) else 0x1000)
                final_factor = 1 if row['final_double_speed'] else 2
                self.assertEqual(b['steady_resume_call_gap_8mhz_clocks'], 4 * final_factor)
                if probe.split(row['scenario'])[1] == 'nop':
                    self.assertTrue(row['match'])
                    self.assertEqual(b['first_resume_call_gap_8mhz_clocks'], b['nominal_resume_8mhz_clocks'])
            rom = root / 'results/active-switch-normal.gbc'
            boot = root / 'results/owned-cgb-boot.bin'
            for arguments in (('0x1000', '0x1000', 'cgb-e', '20000000'),
                              ('65536', '0x1003', 'cgb-e', '20000000'),
                              ('0x1000', '0x1003', 'invalid', '20000000'),
                              ('0x1000', '0x1003', 'cgb-e', '0')):
                result = subprocess.run([str(adapter), '--pc-clock-window', str(rom), str(boot), *arguments],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 2)
            timeout = subprocess.run([str(adapter), '--pc-clock-window', str(rom), str(boot),
                                      '0x1000', '0x1003', 'cgb-e', '1'], capture_output=True, text=True, timeout=30)
            self.assertEqual(timeout.returncode, 3)
            data = json.loads(timeout.stdout)
            self.assertEqual(data['result'], 'timeout')
            self.assertFalse(data['exit_seen'])
            self.assertLessEqual(len(data['callback_trace']), 8)
            with self.assertRaises(ValueError):
                probe.reference_window(timeout, 'active-switch', False)


if __name__ == '__main__':
    unittest.main()
