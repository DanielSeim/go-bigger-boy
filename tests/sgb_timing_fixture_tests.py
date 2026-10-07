#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned timing transport and bounded, sanitized key-on/off observations."""
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
from build_sgb_timing_fixture import build, score_payload, CASES
from check_sgb_timing_reference import observe, check_matrix, run
from decode_sgb_score import decode

HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(interval=87000, gate=74000):
    events = [(0, 0x3D, 0), (1, 0x24, 2)]
    for index, pitch in enumerate((1068, 1132, 2140)):
        onset = 1000 + index*interval
        events.extend(((onset-2, 0x22, pitch & 255), (onset-1, 0x23, pitch >> 8),
                       (onset, 0x4C, 4), (onset+gate, 0x5C, 4)))
    return HEADER + ''.join(f'D,0,{cycle},0,{address},{value}\n' for cycle, address, value in events)


def matrix():
    return [{'model': model, 'case': case,
             **observe(io.StringIO(trace(43500 if case == 'double-tempo' else 87000,
                                          {'baseline': 74000, 'double-tempo': 36000, 'short-gate': 47000}[case])))}
            for case in CASES for model in ('sgb', 'sgb2')]


class TimingFixtures(unittest.TestCase):
    def test_transport_patterns_duration_and_control_isolation(self):
        payloads = []
        for case, (tempo, articulation) in CASES.items():
            rom, payload = build(case), score_payload(case)
            payloads.append(payload)
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x4000:0x5000], payload)
            self.assertEqual((sum(rom[0x134:0x14E]) + 25) & 255, 0)
            self.assertEqual(int.from_bytes(rom[0x14E:0x150], 'big'), sum(rom[:0x14E]+rom[0x150:]) & 65535)
            self.assertEqual(struct.unpack_from('<HH', payload), (54, 0x2B00))
            self.assertEqual(struct.unpack_from('<HH', payload, 58), (0, 0x0400))
            bank = payload[4:58]
            self.assertEqual(struct.unpack_from('<8H', bank, 20), (0, 0, 0x2B24, 0, 0, 0, 0, 0))
            self.assertEqual(bank[36:], bytes((0xE0, 2, 0xE1, 10, 0xE5, 160, 0xED, 127,
                             0xE7, tempo, 16, articulation, 0x98, 0x99, 0xA4, 1, 0xC9, 0)))
            result = decode(bank, 0x2B10)
            self.assertEqual(len(result['patterns']), 1)
            self.assertEqual(payload[62:], bytes(4096-62))
        self.assertEqual([i for i, (a,b) in enumerate(zip(payloads[0],payloads[1])) if a!=b], [49])
        self.assertEqual([i for i, (a,b) in enumerate(zip(payloads[0],payloads[2])) if a!=b], [51])
        with self.assertRaises(ValueError):
            build('missing')

    def test_metadata_and_gate_pairing(self):
        result = observe(io.StringIO(trace()+'R,0,999999,0,100,private sample sentinel\n'))
        self.assertEqual(result, {'voice': 2, 'base_notes': [24,25,36],
                                 'onset_intervals_spc_cycles': [87000,87000],
                                 'gate_spc_cycles': [74000,74000,74000]})
        self.assertNotIn('sentinel', json.dumps(result))
        # Repeated high KOF writes retain the first key-off, not the last one.
        data = trace().replace('D,0,75000,0,92,4\n', 'D,0,75000,0,92,4\nD,0,76000,0,92,4\n')
        self.assertEqual(observe(io.StringIO(data)), result)

    def test_rejects_missing_retriggered_noisy_or_malformed_notes(self):
        data = trace()
        invalid = [HEADER, 'bad header\n', data.replace('D,0,75000,0,92,4\n', ''),
                   data.replace('D,0,249000,0,92,4\n', ''), data+'D,0,300000,0,76,4\n',
                   data.replace(',76,4\n', ',76,8\n'), data.replace('D,0,0,0,61,0', 'D,0,0,0,61,4'),
                   data.replace('D,0,1,0,36,2', 'D,0,1,0,36,10'),
                   data.replace('D,0,998,0,34,44', 'D,0,998,0,34,45'),
                   data.replace('D,0,75000,', 'D,0,1000,'), data.replace('D,0,999,', 'D,0,997,'),
                   HEADER+'D,0,-1,0,34,0\n', HEADER+'D,0,0,0,128,0\n',
                   HEADER+'D,0,0,0,34,256\n', HEADER+'D,0,0,0,34\n',
                   HEADER+'D,0,0,0,34,0,extra\n']
        for changed in invalid:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                observe(io.StringIO(changed))
        with patch('check_sgb_timing_reference.MAX_ROWS', 2), self.assertRaises(ValueError):
            observe(io.StringIO(HEADER+'R,0,0,0,0,private\n'*2))

    def test_matrix_rejects_tempo_gate_or_model_regressions(self):
        good = matrix()
        self.assertEqual(len(check_matrix(good)), 2)
        invalid = [good[:-1]]
        malformed = deepcopy(good)
        malformed[0]['gate_spc_cycles'] = []
        invalid.append(malformed)
        for case, field, value in (
                ('baseline', 'onset_intervals_spc_cycles', [50000,50000]),
                ('double-tempo', 'onset_intervals_spc_cycles', [87000,87000]),
                ('short-gate', 'onset_intervals_spc_cycles', [70000,70000]),
                ('short-gate', 'gate_spc_cycles', [74000]*3),
                ('double-tempo', 'gate_spc_cycles', [39000]*3)):
            changed = deepcopy(good)
            next(r for r in changed if r['model']=='sgb2' and r['case']==case)[field] = value
            invalid.append(changed)
        for changed in invalid:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                check_matrix(changed)

    def test_child_output_sanitization(self):
        def child(command, **kwargs):
            Path(command[-1]).write_text(trace()+'R,0,999999,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command, 4, b'private stdout sentinel', b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            (base/'sgb1.program.rom').write_bytes(b'original test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_timing_reference.subprocess.run', side_effect=child):
                result=run(Path('unused-trace'), base, 'sgb', 'baseline')
            self.assertEqual(set(result), {'model','case','tempo','articulation','duration','voice','base_notes',
                'onset_intervals_spc_cycles','gate_spc_cycles','fixture_sha256','gb_boot_sha256','program_sha256','ipl_sha256'})
            self.assertNotIn('sentinel', json.dumps(result))
            failed=subprocess.CompletedProcess([],3,b'private stdout sentinel',b'private stderr sentinel')
            with patch('check_sgb_timing_reference.subprocess.run', return_value=failed), self.assertRaises(ValueError) as error:
                run(Path('unused-trace'),base,'sgb','baseline')
            self.assertEqual(str(error.exception),'sgb: reference did not finish at the instruction bound')

    def test_cli_no_overwrite_invalid_case_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'fixture.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_timing_fixture.py'),'--output',str(path)]
            subprocess.run(command,check=True,capture_output=True)
            expected=path.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(path.read_bytes(),expected)
            path.unlink()
            self.assertEqual(subprocess.run(command+['--case','missing'],capture_output=True).returncode,2)
            self.assertFalse(path.exists())
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_timing_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')


if __name__ == '__main__':
    unittest.main()
