#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cold mixed failure counters, RAM replacement, first audio and replay guards."""
import copy
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_cold_mixed_audio_fixture import build,ORDERS,PROFILES,failures
from build_sgb_bank_cold_audio_fixture import replacement,pack,objects
from check_sgb_bank_cold_mixed_audio import observe,compare
from check_sgb_bank_cold_audio import fnv
from sgb_bank_cold_tail_audio_tests import synthetic as tail_synthetic


def synthetic(order='gap-root',profile='both',voice=2,model='sgb'):
    m,raw=tail_synthetic('repeat-tail',profile,voice,model)
    first,second=failures(order)
    m['rejected_upload']=first;m['mixed_rejection']=True
    for key in ('semantic_tail','repeated_rejection','consumed_tokens'):m.pop(key)
    score,asset=objects('both',voice)[1]
    failed=[fnv(bytes(2)+score[2:]),fnv(asset)];zero=[fnv(bytes(2048)),fnv(bytes(192))]
    for i,e in enumerate(m['rejects']):
        kind=second if i>=3 else first
        e[5]=1 if kind=='asset-gap' else 2
        e[6]=1 if i>=3 or first=='bad-root' else 0
        e[15:]=zero if kind=='asset-gap' else failed
    m['clears'][1][0]=0 if first=='asset-gap' else 1
    m['clears'][2][0]=1
    m['banks'][0][0]=2;m['recovered'][2]=2
    return m,raw


def controls(order='gap-root',voice=2,model='sgb'):
    return {p:observe(*synthetic(order,p,voice,model),order,p,voice) for p in PROFILES}


class ColdMixedAudioTests(unittest.TestCase):
    def test_three_physical_payloads_and_isolated_sources(self):
        for order in ORDERS:
            first,second=failures(order)
            for voice in (2,3):
                for profile in PROFILES:
                    image=build(order,profile,voice)
                    self.assertEqual(image,build(order,profile,voice));self.assertEqual(len(image),32768)
                    self.assertEqual(image[0x4000:0x5000],replacement(first,voice))
                    self.assertEqual(image[0x5000:0x6000],replacement(second,voice))
                    self.assertEqual(image[0x6000:0x7000],pack(*objects('gb' if profile=='gb' else 'both',voice)[1]))
                    self.assertFalse(any(image[0x7000:]))
                    both=build(order,'both',voice)
                    changes=[(a,b) for a,b in zip(both[0x150:0x4000],image[0x150:0x4000]) if a!=b]
                    self.assertEqual(changes,[(16,0)] if profile=='native' else [])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('bad',),('gap-root','bad'),('gap-root','both',True),('root-gap','both',4)):
            with self.assertRaises(ValueError):build(*args)
        with tempfile.TemporaryDirectory() as name:
            output=Path(name)/'cold-mixed.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_cold_mixed_audio_fixture.py'),
                     '--order','root-gap','--voice','3','--profile','native','--output',str(output)]
            for code in (0,2):
                r=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(r.returncode,code,r.stderr);self.assertEqual(output.read_bytes(),build('root-gap','native',3))

    def test_both_orders_models_and_voices(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for order in ORDERS:
                    result=compare(controls(order,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(len(result['gb_pulse_windows']),7)
                    self.assertTrue(result['first_bank_admitted'])

    def test_stale_error_handoff_ram_and_missing_replay(self):
        for order in ORDERS:
            original,raw=synthetic(order)
            for mutation in ('identity','cold','fault','sounds','edges','clears','rejects','queued_clear','queued_failure',
                             'first_handoff','second_handoff','second_error','second_count','second_suppression',
                             'second_block','second_ready','second_score','second_asset','clear_generation',
                             'publication_count','publication_generation','publication_hash','admission_generation',
                             'admission_error','admission_count','admission_block','early_note','extra_note','bool'):
                m=copy.deepcopy(original)
                if mutation=='identity':m['mixed_rejection']=False
                elif mutation=='cold':m['cold_rejection']=False
                elif mutation=='fault':m['rejected_upload']=failures(order)[1]
                elif mutation=='sounds':m['sounds']=4
                elif mutation=='edges':m['restore_edges']=127
                elif mutation=='clears':m['clears'].pop()
                elif mutation=='rejects':m['rejects'].pop()
                elif mutation=='queued_clear':m['queued_clears']=3
                elif mutation=='queued_failure':m['queued_rejects']=15
                elif mutation=='first_handoff':m['rejects'][0][6]+=1
                elif mutation=='second_handoff':m['rejects'][3][6]+=1
                elif mutation=='second_error':m['rejects'][3][5]=m['rejects'][0][5]
                elif mutation=='second_count':m['rejects'][3][0]=1
                elif mutation=='second_suppression':m['rejects'][4][1]=2
                elif mutation=='second_block':m['rejects'][4][4]=0
                elif mutation=='second_ready':m['rejects'][4][9]=1
                elif mutation=='second_score':m['rejects'][4][15]=m['rejects'][0][15]
                elif mutation=='second_asset':m['rejects'][4][16]=m['rejects'][0][16]
                elif mutation=='clear_generation':m['clears'][2][0]=2
                elif mutation=='publication_count':m['banks'].append(m['banks'][0].copy())
                elif mutation=='publication_generation':m['banks'][0][0]=3
                elif mutation=='publication_hash':m['banks'][0][3]=0
                elif mutation=='admission_generation':m['recovered'][2]=3
                elif mutation=='admission_error':m['recovered'][7]=1
                elif mutation=='admission_count':m['recovered'][3]=1
                elif mutation=='admission_block':m['recovered'][5]=1
                elif mutation=='early_note':m['notes'][0][6]=98000
                elif mutation=='extra_note':m['notes'].append(m['notes'][0].copy())
                else:m['rejects'][4][6]=True
                with self.subTest(order=order,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,order,'both',2)

    def test_native_leakage_and_common_gb_dropouts(self):
        for order in ORDERS:
            m,raw=synthetic(order)
            for frame in (1000,20000,85128,100128,113228,122128,136138,173999,184000):
                changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
                with self.subTest(order=order,frame=frame),self.assertRaises(ValueError):observe(m,bytes(changed),order,'both',2)
        for begin in (60000,73100,76100,85000,113100,122000):
            c=controls()
            for profile in ('both','gb'):c[profile]['left'][begin:begin+4000]=[0]*4000
            with self.subTest(begin=begin),self.assertRaises(ValueError):compare(c,'sgb',2)
        for mutation in ('native_left','gb_right','mix','timeline','silent'):
            c=controls()
            if mutation=='native_left':c['native']['left'][174500]=1
            elif mutation=='gb_right':c['gb']['right'][174500]=1
            elif mutation=='mix':c['both']['right'][174500]+=5
            elif mutation=='timeline':c['native']['meta']['recovered'][0]+=1
            else:
                for profile in ('both','native'):c[profile]['right']=[0]*len(c[profile]['right'])
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
