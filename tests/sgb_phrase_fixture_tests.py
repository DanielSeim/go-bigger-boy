#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned two-channel patterns and sanitized reference transition contracts."""
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
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_phrase_fixture import build, score_payload, CASES
from check_sgb_phrase_reference import observe, check_contract, run
from decode_sgb_score import decode

HEADER='kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(interval=88000):
    events=[(0,0x3D,0),(1,0x24,2),(2,0x34,2)]
    for pattern,pitches in enumerate(((1068,1132),(2140,2140))):
        start=1000+pattern*interval
        for i,(voice,pitch) in enumerate(zip((2,3),pitches)):
            events.extend(((start+2*i,16*voice+2,pitch&255),(start+2*i+1,16*voice+3,pitch>>8)))
        events.append((start+4,0x4C,12))
    return HEADER+''.join(f'D,0,{cycle},0,{address},{value}\n' for cycle,address,value in events)


class PhraseFixtures(unittest.TestCase):
    def test_transport_patterns_durations_and_pins(self):
        pins={'short-first':'7d79c2c3f49d4cc7ba01755fcc0b4a667852c671bad681c24373d429bca0f922',
              'long-first':'34fe6ecb5b07e91b50cce483ebd8f81bb110551aeff12b87050d5c2d0250b4f6',
              'both-long':'48b1f9af9fc605864cce6d8a3f2d6882533d8d0f533d1ead5771ee560087efd6'}
        for case,durations in CASES.items():
            rom,payload=build(case),score_payload(case)
            self.assertEqual(hashlib.sha256(rom).hexdigest(),pins[case])
            self.assertEqual(rom[0x4000:0x5000],payload)
            self.assertEqual((sum(rom[0x134:0x14E])+25)&255,0)
            self.assertEqual(int.from_bytes(rom[0x14E:0x150],'big'),sum(rom[:0x14E]+rom[0x150:])&65535)
            self.assertEqual(struct.unpack_from('<HH',payload),(108,0x2B00))
            self.assertEqual(struct.unpack_from('<HH',payload,112),(0,0x0400))
            bank=payload[4:112]
            self.assertEqual(struct.unpack_from('<HHH',bank,16),(0x2B20,0x2B30,0))
            for pattern in (0,1):
                channels=struct.unpack_from('<8H',bank,32+16*pattern)
                self.assertEqual([i for i,pointer in enumerate(channels) if pointer],[2,3])
                for channel in (2,3):
                    offset=channels[channel]-0x2B00
                    controls=bytes((0xE0,2,0xE1,10,0xED,127))
                    if pattern==0 and channel==2:
                        controls+=bytes((0xE5,160,0xE7,96))
                    duration=durations[channel-2] if pattern==0 else 16
                    note=(24 if channel==2 else 25) if pattern==0 else 36
                    stream=controls+bytes((duration,0x7F,0x80+note,0))
                    self.assertEqual(bank[offset:offset+len(stream)],stream)
            self.assertEqual(len(decode(bank,0x2B10)['patterns']),2)
            self.assertEqual(payload[116:],bytes(4096-116))
        with self.assertRaises(ValueError):
            build('missing')

    def test_pitch_routing_metadata_and_timing(self):
        result=observe(io.StringIO(trace()+'R,0,99999,0,100,private sample sentinel\n'))
        self.assertEqual(result['pattern_interval_spc_cycles'],88000)
        self.assertEqual([event['mask'] for event in result['keyons']],[12,12])
        self.assertEqual([[v['pitch'] for v in e['voices']] for e in result['keyons']],[[1068,1132],[2140,2140]])
        self.assertNotIn('sentinel',json.dumps(result))
        check_contract(result,'short-first')
        check_contract(result,'long-first')
        check_contract(observe(io.StringIO(trace(175000))),'both-long')
        for case,interval in (('short-first',175000),('long-first',175000),('both-long',88000)):
            with self.assertRaises(ValueError):
                check_contract(observe(io.StringIO(trace(interval))),case)

    def test_rejects_partial_split_noisy_or_malformed_keyons(self):
        data=trace()
        invalid=[HEADER,'bad header\n',data.replace('D,0,89004,0,76,12\n',''),
                 data+'D,0,200000,0,76,12\n',data.replace(',76,12\n',',76,4\n'),
                 data.replace('D,0,0,0,61,0','D,0,0,0,61,12'),
                 data.replace('D,0,2,0,52,2','D,0,2,0,52,10'),
                 data.replace('D,0,1002,0,50,108','D,0,1002,0,50,109'),
                 data.replace('D,0,1003,0,51,4\n',''),data.replace('D,0,1003,','D,0,1001,'),
                 HEADER+'D,0,-1,0,34,0\n',HEADER+'D,0,0,0,128,0\n',
                 HEADER+'D,0,0,0,34,256\n',HEADER+'D,0,0,0,34\n',HEADER+'D,0,0,0,34,0,extra\n']
        for changed in invalid:
            with self.subTest(changed=changed),self.assertRaises(ValueError):
                observe(io.StringIO(changed))
        with patch('check_sgb_phrase_reference.MAX_ROWS',2),self.assertRaises(ValueError):
            observe(io.StringIO(HEADER+'R,0,0,0,0,private\n'*2))

    def test_child_success_and_failure_output_sanitization(self):
        def child(command,**kwargs):
            Path(command[-1]).write_text(trace()+'R,0,99999,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command,4,b'private stdout sentinel',b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            (base/'sgb1.program.rom').write_bytes(b'original test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_phrase_reference.subprocess.run',side_effect=child):
                result=run(Path('unused-trace'),base,'sgb','short-first')
            self.assertEqual(set(result),{'model','case','first_pattern_durations','keyons','pattern_interval_spc_cycles',
                'fixture_sha256','gb_boot_sha256','program_sha256','ipl_sha256'})
            self.assertNotIn('sentinel',json.dumps(result))
            failed=subprocess.CompletedProcess([],3,b'private stdout sentinel',b'private stderr sentinel')
            with patch('check_sgb_phrase_reference.subprocess.run',return_value=failed),self.assertRaises(ValueError) as error:
                run(Path('unused-trace'),base,'sgb','short-first')
            self.assertEqual(str(error.exception),'sgb: reference did not finish at the instruction bound')

    def test_cli_no_overwrite_invalid_case_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'fixture.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_phrase_fixture.py'),'--output',str(path)]
            subprocess.run(command,check=True,capture_output=True)
            expected=path.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(path.read_bytes(),expected)
            path.unlink()
            self.assertEqual(subprocess.run(command+['--case','missing'],capture_output=True).returncode,2)
            self.assertFalse(path.exists())
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_phrase_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')


if __name__=='__main__':
    unittest.main()
