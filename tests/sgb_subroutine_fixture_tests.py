#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned finite calls and strict, sanitized reference repetition/return reports."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_subroutine_fixture import build,score_payload,CASES
from check_sgb_subroutine_reference import observe,check_contract,run

HEADER='kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(pitches=(1068,1132,2140,1068),interval=87000):
    events=[(0,0x3D,0),(1,0x24,2)]
    for index,pitch in enumerate(pitches):
        start=1000+index*interval
        events.extend(((start,0x22,pitch&255),(start+1,0x23,pitch>>8),(start+2,0x4C,4)))
    return HEADER+''.join(f'D,0,{cycle},0,{address},{value}\n' for cycle,address,value in events)


class SubroutineFixtures(unittest.TestCase):
    def test_transport_call_target_counts_body_and_return(self):
        pins={'once':'17a68151d643926e8bf0e9ee96c52bf93a8db7f2f6e93f4aeb65549a663b8e4b',
              'twice':'a71dc5d39c2c792a1f960a2ae5ccdab256f05678a9702cd130a12007ff8d420e',
              'thrice':'a4a35d67d7d2c75bf941528effa57e76d19611eee85338fa9adfc058ea775638'}
        payloads=[]
        for case,count in CASES.items():
            rom,payload=build(case),score_payload(case)
            payloads.append(payload)
            self.assertEqual(hashlib.sha256(rom).hexdigest(),pins[case])
            self.assertEqual(rom[0x4000:0x5000],payload)
            self.assertEqual((sum(rom[0x134:0x14E])+25)&255,0)
            self.assertEqual(int.from_bytes(rom[0x14E:0x150],'big'),sum(rom[:0x14E]+rom[0x150:])&65535)
            self.assertEqual(struct.unpack_from('<HH',payload),(69,0x2B00))
            self.assertEqual(struct.unpack_from('<HH',payload,73),(0,0x0400))
            bank=payload[4:73]
            self.assertEqual(struct.unpack_from('<HH',bank,16),(0x2B14,0))
            self.assertEqual(struct.unpack_from('<8H',bank,20),(0,0,0x2B24,0,0,0,0,0))
            self.assertEqual(bank[36:53],bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,96,
                                              0xEF,0x40,0x2B,count,0xA4,0x98,0)))
            self.assertEqual(bank[53:64],bytes(11))
            self.assertEqual(bank[64:],bytes((16,0x7F,0x98,0x99,0)))
            self.assertEqual(payload[77:],bytes(4096-77))
        for payload in payloads[1:]:
            self.assertEqual([i for i,(a,b) in enumerate(zip(payloads[0],payload)) if a!=b],[53])
        with self.assertRaises(ValueError):
            build('missing')

    def test_exact_repeat_and_return_with_duration_timing(self):
        for case,count in CASES.items():
            pitches=[1068,1132]*count+[2140,1068]
            result=observe(io.StringIO(trace(pitches)+'R,0,999999,0,100,private sample sentinel\n'))
            check_contract(result,case)
            self.assertEqual(result['pitches'],pitches)
            self.assertEqual(result['onset_intervals_spc_cycles'],[87000]*(len(pitches)-1))
            self.assertNotIn('sentinel',json.dumps(result))
        for pitches in ((1068,1132),(1068,2140,1132),(1068,1132,1068,1132,2140)):
            with self.assertRaises(ValueError):
                check_contract(observe(io.StringIO(trace(pitches))),'once')
        for interval in (43000,175000):
            with self.assertRaises(ValueError):
                check_contract(observe(io.StringIO(trace(interval=interval))),'once')
        result=observe(io.StringIO(trace()))
        result['onset_intervals_spc_cycles']=[]
        with self.assertRaises(ValueError):
            check_contract(result,'once')

    def test_malformed_unbounded_or_wrong_voice_events_fail(self):
        data=trace()
        invalid=[HEADER,'bad header\n',trace([1068]*9),data.replace(',76,4\n',',76,8\n'),
                 data.replace('D,0,0,0,61,0','D,0,0,0,61,4'),
                 data.replace('D,0,1,0,36,2','D,0,1,0,36,10'),
                 data.replace('D,0,1000,0,34,44','D,0,1000,0,34,45'),
                 data.replace('D,0,1001,0,35,4\n',''),data.replace('D,0,1001,','D,0,999,'),
                 HEADER+'D,0,-1,0,34,0\n',HEADER+'D,0,0,0,128,0\n',
                 HEADER+'D,0,0,0,34,256\n',HEADER+'D,0,0,0,34\n',HEADER+'D,0,0,0,34,0,extra\n',
                 data+'D,0,262002,0,76,4\n']
        for changed in invalid:
            with self.subTest(changed=changed),self.assertRaises(ValueError):
                observe(io.StringIO(changed))
        with patch('check_sgb_subroutine_reference.MAX_ROWS',2),self.assertRaises(ValueError):
            observe(io.StringIO(HEADER+'R,0,0,0,0,private\n'*2))

    def test_child_success_and_failure_output_sanitization(self):
        def child(command,**kwargs):
            Path(command[-1]).write_text(trace()+'R,0,999999,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command,4,b'private stdout sentinel',b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            (base/'sgb1.program.rom').write_bytes(b'original test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_subroutine_reference.subprocess.run',side_effect=child):
                result=run(Path('unused-trace'),base,'sgb','once')
            self.assertEqual(set(result),{'model','case','call_count','voice','srcn','pitches','onset_intervals_spc_cycles',
                'fixture_sha256','gb_boot_sha256','program_sha256','ipl_sha256'})
            self.assertNotIn('sentinel',json.dumps(result))
            failed=subprocess.CompletedProcess([],3,b'private stdout sentinel',b'private stderr sentinel')
            with patch('check_sgb_subroutine_reference.subprocess.run',return_value=failed),self.assertRaises(ValueError) as error:
                run(Path('unused-trace'),base,'sgb','once')
            self.assertEqual(str(error.exception),'sgb: reference did not finish at the instruction bound')

    def test_cli_no_overwrite_invalid_case_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'fixture.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_subroutine_fixture.py'),'--output',str(path)]
            subprocess.run(command,check=True,capture_output=True)
            expected=path.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(path.read_bytes(),expected)
            path.unlink()
            self.assertEqual(subprocess.run(command+['--case','missing'],capture_output=True).returncode,2)
            self.assertFalse(path.exists())
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_subroutine_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')


if __name__=='__main__':
    unittest.main()
