#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cold mixed one-byte token transitions, physical payloads and replay guards."""
import copy
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_cold_mixed_audio_fixture import build,ORDERS,PROFILES,failures
from build_sgb_bank_cold_audio_fixture import objects
from check_sgb_bank_cold_mixed_tail_audio import fixture,observe,compare
from sgb_bank_cold_mixed_audio_tests import synthetic as mixed_synthetic


def synthetic(order='gap-root',profile='both',voice=2,model='sgb'):
    m,raw=mixed_synthetic(order,profile,voice,model)
    m.update(semantic_tail=True,repeated_rejection=True,
             consumed_tokens=[1,1,1,3,3] if order=='gap-root' else [3,3,3,4,4])
    return m,raw


def controls(order='gap-root',voice=2,model='sgb'):
    return {p:observe(*synthetic(order,p,voice,model),order,p,voice) for p in PROFILES}


class ColdMixedTailAudioTests(unittest.TestCase):
    def test_one_byte_semantic_payload_and_unchanged_preflight(self):
        for order in ORDERS:
            index=1 if order=='gap-root' else 0
            for voice in (2,3):
                score,asset=objects('both',voice)[1]
                data=(struct.pack('<HH',2048,0x2B00)+bytes(2)+score[2:]+struct.pack('<HH',191,0x5000)+asset[:191]+
                      struct.pack('<HH',1,0x50BF)+asset[191:]+struct.pack('<HH',0,0x0400))
                for profile in PROFILES:
                    image=fixture(order,profile,voice);old=build(order,profile,voice)
                    self.assertEqual(image,fixture(order,profile,voice));self.assertEqual(len(image),32768)
                    begin=0x4000+4096*index
                    self.assertEqual(image[begin:begin+4096],data+bytes(4096-len(data)))
                    other=0x4000+4096*(1-index)
                    self.assertEqual(image[other:other+4096],old[other:other+4096])
                    self.assertEqual(image[0x6000:],old[0x6000:])
                    self.assertEqual(image[0x150:0x4000],old[0x150:0x4000])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for value in (1,None,'yes'):
            with self.assertRaises(ValueError):build(semantic_tail=value)
        with self.assertRaises(ValueError):fixture('bad','both',2)
        with tempfile.TemporaryDirectory() as name:
            output=Path(name)/'cold-mixed-tail.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_cold_mixed_audio_fixture.py'),
                     '--order','root-gap','--semantic-tail','--voice','3','--profile','native','--output',str(output)]
            for code in (0,2):
                result=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,code,result.stderr);self.assertEqual(output.read_bytes(),fixture('root-gap','native',3))

    def test_both_models_voices_and_failure_orders(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for order in ORDERS:
                    result=compare(controls(order,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(len(result['gb_pulse_windows']),7)
                    self.assertTrue(result['first_bank_admitted'])

    def test_token_transition_and_strict_identity(self):
        for order in ORDERS:
            original,raw=synthetic(order)
            for mutation in ('missing','extra','all_three','reversed','bool','string','missing_tail','wrong_repeat',
                             'bool_repeat','missing_mixed','wrong_fault','missing_cold'):
                m=copy.deepcopy(original)
                if mutation=='missing':m['consumed_tokens'].pop()
                elif mutation=='extra':m['consumed_tokens'].append(3)
                elif mutation=='all_three':m['consumed_tokens']=[3]*5
                elif mutation=='reversed':m['consumed_tokens'].reverse()
                elif mutation=='bool':m['consumed_tokens'][0]=True
                elif mutation=='string':m['consumed_tokens'][0]='3'
                elif mutation=='missing_tail':m.pop('semantic_tail')
                elif mutation=='wrong_repeat':m['repeated_rejection']=False
                elif mutation=='bool_repeat':m['repeated_rejection']=1
                elif mutation=='missing_mixed':m.pop('mixed_rejection')
                elif mutation=='wrong_fault':m['rejected_upload']=failures(order)[1]
                else:m['cold_rejection']=False
                with self.subTest(order=order,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,order,'both',2)
            for index in range(5):
                m=copy.deepcopy(original);m['consumed_tokens'][index]+=1
                with self.subTest(order=order,index=index),self.assertRaises(ValueError):observe(m,raw,order,'both',2)

    def test_synchronized_token_cannot_hide_stale_failure_or_replay(self):
        for order in ORDERS:
            original,raw=synthetic(order)
            for mutation in ('error','handoff','ram','counter','blocked','clear','bank','admission','queued','note'):
                m=copy.deepcopy(original)
                if mutation=='error':m['rejects'][3][5]=m['rejects'][0][5]
                elif mutation=='handoff':m['rejects'][3][6]=2
                elif mutation=='ram':m['rejects'][4][15]=m['rejects'][0][15]
                elif mutation=='counter':m['rejects'][4][1]=2
                elif mutation=='blocked':m['rejects'][4][4]=0
                elif mutation=='clear':m['clears'][2][0]=2
                elif mutation=='bank':m['banks'][0][0]=3
                elif mutation=='admission':m['recovered'][7]=1
                elif mutation=='queued':m['queued_rejects']=15
                else:m['notes'][0][6]=98000
                with self.subTest(order=order,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,order,'both',2)


if __name__=='__main__':unittest.main()
