#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned mixed failures, error/RAM transitions and audible recovery guards."""
import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_mixed_audio_fixture import build,failures,ORDERS,PROFILES,pack,objects,replacement
from build_sgb_score_atomic_fixture import build_cartridge
from check_sgb_bank_mixed_audio import observe,compare
from check_sgb_bank_replace_audio import fnv
spec=importlib.util.spec_from_file_location('tail_guards',Path(__file__).with_name('sgb_bank_tail_audio_tests.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)


def synthetic(order='gap-root',profile='both',voice=2,model='sgb'):
    m,raw=base.synthetic('repeat-tail',profile,voice,model)
    first,second=failures(order)
    for key in ('semantic_tail','repeated_rejection','consumed_tokens'):del m[key]
    m.update(mixed_rejection=True,rejected_upload=first)
    score,asset=objects('both',voice)[1]
    hashes={'asset-gap':[fnv(bytes(2048)),fnv(bytes(192))],
            'bad-root':[fnv(bytes(2)+score[2:]),fnv(asset)]}
    generation=1 if first=='asset-gap' else 2
    for i,e in enumerate(m['rejects']):
        fault=first if i<3 else second
        e[5]=1 if fault=='asset-gap' else 2
        e[6]=generation if i<3 else 2
        e[15:]=hashes[fault]
    m['clears'][2][0]=generation;m['clears'][3][0]=2
    m['banks'][1][0]=3;m['recovered'][2]=3
    return m,raw


def controls(order='gap-root',voice=2,model='sgb'):
    return {p:observe(*synthetic(order,p,voice,model),order,p,voice) for p in PROFILES}


class MixedAudioTests(unittest.TestCase):
    def test_exact_four_payloads_and_bounded_export(self):
        for order in ORDERS:
            for voice in (2,3):
                for profile in PROFILES:
                    image=build(order,profile,voice)
                    self.assertEqual(image,build(order,profile,voice));self.assertEqual(len(image),32768)
                    first,second=failures(order)
                    payloads=(pack(*objects(profile,voice)[0]),replacement(first,voice),
                              replacement(second,voice),pack(*objects(profile,voice)[1]))
                    for i,value in enumerate(payloads):self.assertEqual(image[0x4000+4096*i:0x5000+4096*i],value)
                    self.assertEqual(image[0x150:0x4000],build(order,'both',voice)[0x150:0x4000])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('bad',),('gap-root','bad'),('gap-root','both',True),('gap-root','both',4)):
            with self.assertRaises(ValueError):build(*args)
        with self.assertRaises(ValueError):build_cartridge([bytes(4096)]*5,[(1,bytes((0x41,)),None)])
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'mixed.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_mixed_audio_fixture.py'),
                     '--order','root-gap','--profile','old','--voice','3','--output',str(path)]
            for code in (0,2):
                result=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,code,result.stderr);self.assertEqual(path.read_bytes(),build('root-gap','old',3))

    def test_both_orders_models_and_voices(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for order in ORDERS:
                    result=compare(controls(order,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(len(result['gb_recovery_windows']),6)
                    self.assertTrue(result['fresh_restart_audible'])

    def test_stale_error_ram_generation_admission_and_replay_rejected(self):
        for order in ORDERS:
            original,raw=synthetic(order)
            for mutation in ('identity','bool_identity','order','error','first_error','generation','first_generation',
                             'stale_ram','first_ram','missing_event','rejections','suppressions','blocked','ready',
                             'missing_clear','clear_generation','clear_time','queued_event','queued_clear','publication',
                             'admission','admit_error','admit_count','admit_blocked','admit_hash'):
                m=copy.deepcopy(original)
                if mutation=='identity':m['mixed_rejection']=False
                elif mutation=='bool_identity':m['mixed_rejection']=1
                elif mutation=='order':m['rejected_upload']=failures(order)[1]
                elif mutation=='error':m['rejects'][3][5]=m['rejects'][0][5]
                elif mutation=='first_error':m['rejects'][0][5]=m['rejects'][3][5]
                elif mutation=='generation':m['rejects'][3][6]+=1
                elif mutation=='first_generation':m['rejects'][0][6]+=1
                elif mutation=='stale_ram':m['rejects'][3][15:]=m['rejects'][0][15:]
                elif mutation=='first_ram':m['rejects'][0][15:]=m['rejects'][3][15:]
                elif mutation=='missing_event':m['rejects'].pop(3)
                elif mutation=='rejections':m['rejects'][3][0]=1
                elif mutation=='suppressions':m['rejects'][4][1]=2
                elif mutation=='blocked':m['rejects'][3][4]=0
                elif mutation=='ready':m['rejects'][3][9]=1
                elif mutation=='missing_clear':m['clears'].pop(2)
                elif mutation=='clear_generation':m['clears'][2][0]+=1
                elif mutation=='clear_time':m['clears'][2][1]=119000
                elif mutation=='queued_event':m['queued_rejects']=7
                elif mutation=='queued_clear':m['queued_clears']=7
                elif mutation=='publication':m['banks'][1][0]=4
                elif mutation=='admission':m['recovered'][2]=4
                elif mutation=='admit_error':m['recovered'][7]=1
                elif mutation=='admit_count':m['recovered'][3]=1
                elif mutation=='admit_blocked':m['recovered'][5]=1
                else:m['recovered'][16]=0
                with self.subTest(order=order,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,order,'both',2)

    def test_blocked_pcm_and_shared_gb_dropouts_rejected(self):
        for order in ORDERS:
            m,raw=synthetic(order)
            for frame in (80032,90228,94228,110128,114128,118228,130128,170138):
                changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
                with self.subTest(order=order,frame=frame),self.assertRaises(ValueError):observe(m,bytes(changed),order,'both',2)
            for frame in (110000,118100):
                c=controls(order)
                for profile in PROFILES:c[profile]['left'][frame:frame+4000]=[0]*4000
                with self.subTest(order=order,gb_frame=frame),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
