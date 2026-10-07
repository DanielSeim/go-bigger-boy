#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned echo controls and bounded, sanitized note-end DSP observations."""
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from build_sgb_echo_fixture import build, score_payload, CASES
from check_sgb_echo_reference import observe, check_contract, run

HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(case='routing'):
    events = [(0,0x3D,0),(1,0x24,2),(2,0x22,44),(3,0x23,4)]
    for index, (mask,left,right,delay,feedback,fir) in enumerate(CASES[case]):
        start = 1000 + index*20000
        events.append((start,0x4C,4))
        # Echo setup arrives after KON. The observer must retain these updates.
        writes = [(0x4D,mask),(0x2C,left),(0x3C,right),(0x0D,feedback),
                  (0x7D,delay),(0x6D,247 if delay==1 else 239),(0x6C,0)]
        writes.extend((0x0F+16*i,127 if i==0 else 0) for i in range(8))
        events.extend((start+100+i,address,value) for i,(address,value) in enumerate(writes))
        events.append((start+10000,0x5C,4))
    return HEADER+''.join(f'D,0,{cycle},0,{address},{value}\n' for cycle,address,value in events)


class EchoFixtures(unittest.TestCase):
    def test_transport_controls_waits_and_hashes(self):
        pins = {'routing':'f6057f4a933cafcd5bf28801046d24b252599896d6aa84ea1c62fd6a1c4db69c',
                'setup':'1cb1f89c71b44c02314dde4d64ab2c77b61908a4512b0e01131855681b62a84e'}
        for case, controls in CASES.items():
            rom,payload=build(case),score_payload(case)
            self.assertEqual(hashlib.sha256(rom).hexdigest(),pins[case])
            self.assertEqual(rom[0x4000:0x5000],payload)
            self.assertEqual((sum(rom[0x134:0x14E])+25)&255,0)
            self.assertEqual(int.from_bytes(rom[0x14E:0x150],'big'),sum(rom[:0x14E]+rom[0x150:])&65535)
            self.assertEqual(struct.unpack_from('<HH',payload),(104,0x2B00))
            self.assertEqual(struct.unpack_from('<HH',payload,108),(0,0x0400))
            bank=payload[4:108]
            self.assertEqual(struct.unpack_from('<HH',bank,16),(0x2B14,0))
            self.assertEqual(struct.unpack_from('<8H',bank,20),(0,0,0x2B24,0,0,0,0,0))
            self.assertEqual(bank[36:46],bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,96)))
            for i,(mask,left,right,delay,feedback,fir) in enumerate(controls):
                self.assertEqual(bank[46+19*i:65+19*i],bytes((0xF6,64,0x7F,0xC9,0xF7,delay,feedback,fir,
                                 16,0xC9,0xF5,mask,left,right,16,0x7F,0x98,16,0xC9)))
            self.assertEqual(bank[-1],0)
            self.assertEqual(payload[112:],bytes(4096-112))
        with self.assertRaises(ValueError):
            build('missing')

    def test_deferred_echo_setup_and_first_keyoff(self):
        for case in CASES:
            data=trace(case)+'R,0,99999,0,100,private sample sentinel\n'
            result=observe(io.StringIO(data))
            check_contract(result,case)
            self.assertNotIn('sentinel',json.dumps(result))
            self.assertEqual([note['echo']['eon'] for note in result['notes']], [c[0] for c in CASES[case]])
        # A later key-off must not replace an already completed observation.
        data=trace().replace('D,0,11000,0,92,4\n','D,0,11000,0,92,4\nD,0,12000,0,77,255\nD,0,13000,0,92,4\n')
        self.assertEqual(observe(io.StringIO(data)),observe(io.StringIO(trace())))

    def test_contract_rejects_routing_delay_feedback_filter_or_disabled_writes(self):
        for field,value in (('eon',255),('edl',3),('efb',0),('flg',32),('esa',0),('evol_left',0)):
            result=observe(io.StringIO(trace('setup')))
            result['notes'][1]['echo'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):
                check_contract(result,'setup')
        result=observe(io.StringIO(trace()))
        result['notes'][0]['fir'][0]=0
        with self.assertRaises(ValueError):
            check_contract(result,'routing')

    def test_incomplete_retriggered_or_malformed_notes_fail(self):
        data=trace()
        invalid=[HEADER,'bad header\n',data.replace('D,0,11000,0,92,4\n',''),
                 data.replace('D,0,51000,0,92,4\n',''),data+'D,0,60000,0,76,4\n',
                 data.replace(',76,4\n',',76,8\n'),data.replace('D,0,0,0,61,0','D,0,0,0,61,4'),
                 data.replace('D,0,1,0,36,2','D,0,1,0,36,10'),
                 data.replace('D,0,2,0,34,44','D,0,2,0,34,45'),
                 data.replace('D,0,1114,0,127,0\n',''),data.replace('D,0,11000,','D,0,1000,'),
                 HEADER+'D,0,-1,0,34,0\n',HEADER+'D,0,0,0,128,0\n',
                 HEADER+'D,0,0,0,34,256\n',HEADER+'D,0,0,0,34\n',HEADER+'D,0,0,0,34,0,extra\n']
        for changed in invalid:
            with self.subTest(changed=changed),self.assertRaises(ValueError):
                observe(io.StringIO(changed))
        with patch('check_sgb_echo_reference.MAX_ROWS',2),self.assertRaises(ValueError):
            observe(io.StringIO(HEADER+'R,0,0,0,0,private\n'*2))

    def test_success_and_failure_diagnostics_are_sanitized(self):
        def child(command,**kwargs):
            Path(command[-1]).write_text(trace()+'R,0,99999,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command,4,b'private stdout sentinel',b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            (base/'sgb1.program.rom').write_bytes(b'original test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_echo_reference.subprocess.run',side_effect=child):
                result=run(Path('unused-trace'),base,'sgb','routing')
            self.assertEqual(set(result),{'model','case','notes','fixture_sha256','gb_boot_sha256','program_sha256','ipl_sha256'})
            self.assertEqual([n['note'] for n in result['notes']],[1,2,3])
            self.assertNotIn('sentinel',json.dumps(result))
            failed=subprocess.CompletedProcess([],3,b'private stdout sentinel',b'private stderr sentinel')
            with patch('check_sgb_echo_reference.subprocess.run',return_value=failed),self.assertRaises(ValueError) as error:
                run(Path('unused-trace'),base,'sgb','routing')
            self.assertEqual(str(error.exception),'sgb: reference did not finish at the instruction bound')

    def test_cli_no_overwrite_invalid_case_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'fixture.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_echo_fixture.py'),'--output',str(path)]
            subprocess.run(command,check=True,capture_output=True)
            expected=path.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(path.read_bytes(),expected)
            path.unlink()
            self.assertEqual(subprocess.run(command+['--case','missing'],capture_output=True).returncode,2)
            self.assertFalse(path.exists())
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_echo_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')


if __name__=='__main__':
    unittest.main()
