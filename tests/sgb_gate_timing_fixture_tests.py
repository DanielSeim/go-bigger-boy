#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned gate parameter isolation, sanitized observations and matrix rejection checks."""
from copy import deepcopy
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
sys.path.insert(0, str(ROOT/'tests'))
from build_sgb_gate_timing_fixture import CASES, build, score_payload
from build_sgb_timing_fixture import build as old_build, timing_payload
from check_sgb_gate_timing_reference import check_matrix, run
from check_sgb_timing_reference import observe
from schedule_sgb_score import schedule
from sgb_timing_fixture_tests import trace


def matrix():
    results = []
    for model in ('sgb','sgb2'):
        for case, (tempo, art, duration) in CASES.items():
            interval = int(87000*duration/16*96/tempo)
            gate = {'control': 74000, 'half-duration': 31000, 'long-duration': 118000,
                    'half-duration-short': 20000, 'long-duration-short': 76000,
                    'middle-tempo': 55000, 'middle-tempo-short': 34500,
                    'double-tempo-short': 22500}[case]
            results.append({'model': model, 'case': case, 'tempo': tempo,
                            'articulation': art, 'duration': duration,
                            **observe(io.StringIO(trace(interval, gate)))})
    return results


class GateTimingFixtures(unittest.TestCase):
    def test_control_and_parameter_isolation(self):
        self.assertEqual(build('control'), old_build('baseline'))
        control = score_payload('control')
        for case, (tempo, art, duration) in CASES.items():
            with self.subTest(case=case):
                rom, payload = build(case), score_payload(case)
                self.assertEqual(len(rom),32768)
                self.assertEqual(rom[0x4000:0x5000],payload)
                self.assertEqual((sum(rom[0x134:0x14E])+25)&255,0)
                self.assertEqual(int.from_bytes(rom[0x14E:0x150],'big'),sum(rom[:0x14E]+rom[0x150:])&65535)
                self.assertEqual(struct.unpack_from('<HH',payload),(54,0x2B00))
                self.assertEqual(struct.unpack_from('<HH',payload,58),(0,0x0400))
                self.assertEqual(payload[62:],bytes(4096-62))
                expected = {49: tempo,50: duration,51: art}
                self.assertEqual({i for i,(a,b) in enumerate(zip(control,payload)) if a!=b},
                                 {i for i,value in expected.items() if control[i]!=value})
                for offset,value in expected.items():
                    self.assertEqual(payload[offset],value)
                timeline = schedule(payload[4:58],0x2B10)
                notes = [event for event in timeline['events'] if event['kind']=='note']
                self.assertEqual([event['note'] for event in notes],[24,25,36])
                self.assertEqual([event['duration'] for event in notes],[duration]*3)
                self.assertEqual([event['articulation'] for event in notes],[art]*3)
                self.assertEqual(timeline['ticks'],duration*3+1)
        with self.assertRaises(ValueError):
            build('missing')

    def test_invalid_parameters_reject(self):
        for parameters in ((0,127,16),(256,127,16),(True,127,16),(96,-1,16),
                           (96,128,16),(96,127,0),(96,127,128),(96,127,False)):
            with self.subTest(parameters=parameters),self.assertRaises(ValueError):
                timing_payload(*parameters)

    def test_complete_matrix_and_regression_rejection(self):
        good = matrix()
        self.assertEqual(len(check_matrix(good)),14)
        invalid = [None,[],good[:-1],good+[good[0]], [good[0]]*16]
        for case, field, value in (
                ('half-duration','onset_intervals_spc_cycles',[87000]*2),
                ('half-duration','gate_spc_cycles',[37000]*3),
                ('middle-tempo','onset_intervals_spc_cycles',[87000]*2),
                ('half-duration-short','gate_spc_cycles',[39000]*3),
                ('long-duration-short','onset_intervals_spc_cycles',[120000]*2),
                ('control','gate_spc_cycles',[True]*3),
                ('control','gate_spc_cycles',[1,74000,74000]),
                ('control','duration',8), ('control','tempo',128),
                ('control','gate_spc_cycles',[])):
            changed = deepcopy(good)
            next(row for row in changed if row['model']=='sgb2' and row['case']==case)[field] = value
            invalid.append(changed)
        changed = deepcopy(good)
        next(row for row in changed if row['model']=='sgb2' and row['case']=='middle-tempo')['gate_spc_cycles'] = [57500]*3
        invalid.append(changed)
        for changed in invalid:
            with self.subTest(changed=changed),self.assertRaises(ValueError):
                check_matrix(changed)

    def test_child_output_sanitization_and_bounds(self):
        def child(command,**kwargs):
            self.assertEqual(kwargs['timeout'],180)
            self.assertTrue(kwargs['capture_output'])
            self.assertEqual(command[command.index('--instruction-limit')+1],'8000000')
            self.assertIn('--sync-gb-sgb2',command)
            Path(command[-1]).write_text(trace(65250,55000)+'R,0,999999,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command,4,b'private stdout sentinel',b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            (base/'sgb2.program.rom').write_bytes(b'owned test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_timing_reference.subprocess.run',side_effect=child):
                result=run(Path('unused-trace'),base,'sgb2','middle-tempo')
            self.assertEqual((result['tempo'],result['articulation'],result['duration']),(128,127,16))
            self.assertNotIn('sentinel',json.dumps(result))
            failed=subprocess.CompletedProcess([],3,b'private stdout sentinel',b'private stderr sentinel')
            with patch('check_sgb_timing_reference.subprocess.run',return_value=failed),self.assertRaises(ValueError) as error:
                run(Path('unused-trace'),base,'sgb2','middle-tempo')
            self.assertNotIn('sentinel',str(error.exception))

    def test_cli_preserves_output_and_fails_without_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'fixture.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_gate_timing_fixture.py'),
                     '--case','half-duration','--output',str(path)]
            subprocess.run(command,check=True,capture_output=True)
            image=path.read_bytes()
            result=subprocess.run(command,capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(path.read_bytes(),image)
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_gate_timing_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(result.stdout,b'')
            self.assertNotIn(b'sentinel',result.stderr)


if __name__=='__main__':
    unittest.main()
