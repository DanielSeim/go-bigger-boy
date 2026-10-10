#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned one-byte tails, repeated semantic rejection and acoustic guard mutations."""
import copy
import importlib.util
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from check_sgb_bank_tail_audio import fixture,observe,compare,CASES,PROFILES,MASTER,RATE
from build_sgb_bank_recovery_audio_fixture import build,pack,objects
from build_sgb_score_atomic_fixture import build_cartridge
spec=importlib.util.spec_from_file_location('recovery_guards',Path(__file__).with_name('sgb_bank_recovery_audio_tests.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)


def synthetic(kind='tail',profile='both',voice=2,model='sgb'):
    m,raw=base.synthetic('bad-root',profile,voice,model)
    repeat=kind=='repeat-tail';clock=lambda frame:((frame+1)*MASTER+RATE-1)//RATE
    m.update(semantic_tail=True,repeated_rejection=repeat,consumed_tokens=[3]*(5 if repeat else 3))
    if repeat:
        positions=(60000,70000,75000,90000,94000,102000,118000,126000,184000,197000)
        m['edges']=[[i+1,0 if i==9 else 16,f,clock(f)] for i,f in enumerate(positions)]
        n=m['notes'][1]
        for i,f in zip((3,4,5),(188000,197100,197200)):n[i]=f*2048000//RATE
        n[6:9]=[188000,197100,197200]
        m['banks'][1][:3]=[4,170000,clock(170000)]
        m['clears'][2]=[3,130000,clock(130000)]
        m['clears'].insert(2,[2,110000,clock(110000)])
        for count,suppressed,f in ((2,2,114000),(2,3,118100)):
            e=m['rejects'][0].copy();e[:4]=[count,suppressed,f,clock(f)];e[6]=3;m['rejects'].append(e)
        m['recovered'][:5]=[170010,clock(170010),4,2,3]
        m.update(sounds=6,restore_edges=1023,restore_clears=15,queued_clears=15,restore_rejects=31,queued_rejects=31)
        right=[r for _,r in struct.iter_unpack('<hh',raw)]
        for i in range(168000,m['frames']):
            value=math.sin(2*math.pi*440*2**(3/12)*i/RATE)
            right[i]=round(1500*2/math.pi*math.asin(value)) if profile in ('both','new') and n[6]<=i<n[7] else 0
        frequency=(MASTER/5 if model=='sgb' else 4194304)/8192
        raw=b''.join(struct.pack('<hh',(1000 if math.sin(2*math.pi*frequency*i/RATE)>=0 else -1000)
                                if 60000<=i<197000 else 0,right[i]) for i in range(m['frames']))
    return m,raw


def controls(kind='tail',voice=2,model='sgb'):
    return {p:observe(*synthetic(kind,p,voice,model),kind,p,voice) for p in PROFILES}


class TailAudioTests(unittest.TestCase):
    def test_exact_tail_bytes_and_bounded_exports(self):
        for kind in CASES:
            for voice in (2,3):
                for profile in PROFILES:
                    image=fixture(kind,profile,voice)
                    self.assertEqual(image,fixture(kind,profile,voice));self.assertEqual(len(image),32768)
                    self.assertEqual(image[0x4000:0x5000],pack(*objects(profile,voice)[0]))
                    self.assertEqual(image[0x6000:0x7000],pack(*objects(profile,voice)[1]))
                    score,asset=objects('both',voice)[1];bad=image[0x5000:0x6000]
                    expected=(struct.pack('<HH',2048,0x2B00)+bytes(2)+score[2:]+
                              struct.pack('<HH',191,0x5000)+asset[:191]+
                              struct.pack('<HH',1,0x50BF)+asset[191:]+struct.pack('<HH',0,0x400))
                    self.assertEqual(bad,expected+bytes(4096-len(expected)))
                    self.assertEqual(image[0x150:0x4000],fixture(kind,'both',voice)[0x150:0x4000])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for options in (dict(repeat=True),dict(semantic_tail=1),dict(repeat=1),dict(semantic_tail=True)):
            with self.assertRaises(ValueError):build('asset-gap',**options)
        with self.assertRaises(ValueError):fixture('bad','both',2)
        with self.assertRaises(ValueError):build_cartridge([bytes(4096)],[(1,bytes((0x41,)),None)]*11)
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'tail.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_recovery_audio_fixture.py'),
                     '--kind','bad-root','--semantic-tail','--repeat','--profile','new','--voice','3','--output',str(path)]
            for code in (0,2):
                result=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,code,result.stderr);self.assertEqual(path.read_bytes(),fixture('repeat-tail','new',3))

    def test_single_repeated_both_models_and_voices(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in CASES:
                    result=compare(controls(kind,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(len(result['gb_recovery_windows']),6 if kind=='repeat-tail' else 4)
                    self.assertTrue(result['fresh_restart_audible'])

    def test_missing_repeated_event_token_clear_or_admission_rejected(self):
        original,raw=synthetic('repeat-tail')
        for mutation in ('identity','mode','token','bool_token','missing_token','event','reject_count','suppressed_count',
                         'transfer','hash','time','clear','clear_generation','clear_clock','clear_type','queued_event',
                         'queued_clear','edges','publication','admit_rejections','admit_suppressed','admit_generation'):
            m=copy.deepcopy(original)
            if mutation=='identity':m['semantic_tail']=False
            elif mutation=='mode':m['repeated_rejection']=False
            elif mutation=='token':m['consumed_tokens'][3]=2
            elif mutation=='bool_token':m['consumed_tokens'][3]=True
            elif mutation=='missing_token':m['consumed_tokens'].pop()
            elif mutation=='event':m['rejects'].pop(3)
            elif mutation=='reject_count':m['rejects'][3][0]=1
            elif mutation=='suppressed_count':m['rejects'][4][1]=2
            elif mutation=='transfer':m['rejects'][3][6]=2
            elif mutation=='hash':m['rejects'][3][15]=0
            elif mutation=='time':m['rejects'][3][2]=119000
            elif mutation=='clear':m['clears'].pop(2)
            elif mutation=='clear_generation':m['clears'][2][0]=1
            elif mutation=='clear_clock':m['clears'][2][2]=0
            elif mutation=='clear_type':m['clears'][2]=None
            elif mutation=='queued_event':m['queued_rejects']=7
            elif mutation=='queued_clear':m['queued_clears']=7
            elif mutation=='edges':m['restore_edges']=255
            elif mutation=='publication':m['banks'][1][0]=3
            elif mutation=='admit_rejections':m['recovered'][3]=1
            elif mutation=='admit_suppressed':m['recovered'][4]=2
            else:m['recovered'][2]=3
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(m,raw,'repeat-tail','both',2)

    def test_repeat_silence_and_common_gb_dropout(self):
        m,raw=synthetic('repeat-tail')
        for frame in (110128,114128,118228,130128,170138):
            changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
            with self.subTest(frame=frame),self.assertRaises(ValueError):observe(m,bytes(changed),'repeat-tail','both',2)
        for frame in (110000,118100):
            c=controls('repeat-tail')
            for profile in PROFILES:c[profile]['left'][frame:frame+4000]=[0]*4000
            with self.subTest(gb_frame=frame),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
