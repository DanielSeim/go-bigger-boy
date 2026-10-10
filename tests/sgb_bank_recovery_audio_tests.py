#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject premature admission, stale PCM and incomplete recovery replay."""
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
from build_sgb_bank_recovery_audio_fixture import build,pack,objects,FAULTS,PROFILES
from build_sgb_bank_reject_audio_fixture import replacement
from check_sgb_bank_recovery_audio import observe,compare,MASTER,RATE
from check_sgb_bank_replace_audio import fnv
spec=importlib.util.spec_from_file_location('replacement_guards',Path(__file__).with_name('sgb_bank_replace_audio_tests.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)


def synthetic(kind='asset-gap',profile='both',voice=2,model='sgb'):
    m,_=base.synthetic(profile,voice,model,True)
    clock=lambda frame:((frame+1)*MASTER+RATE-1)//RATE
    positions=(60000,70000,75000,90000,94000,102000,164000,177000)
    m['edges']=[[i+1,0 if i==7 else 16,f,clock(f)] for i,f in enumerate(positions)]
    m['notes'][1]=[voice,3,2140,168000*2048000//RATE,177100*2048000//RATE,177200*2048000//RATE,168000,177100,177200,63,1<<voice]
    error=1 if kind=='asset-gap' else 2;generation=error+1
    m['banks'][1][:3]=[generation,150000,clock(150000)]
    m['clears'].append([generation-1,110000,clock(110000)])
    score,asset=objects('both',voice)[1]
    failed=[fnv(bytes(2048)),fnv(bytes(192))] if kind=='asset-gap' else [fnv(bytes(2)+score[2:]),fnv(asset)]
    m.update(sounds=5,restore_edges=255,restore_clears=7,queued_clears=7,rejected_upload=kind,
             restore_rejects=7,queued_rejects=7,recovery_upload=True,restore_recovered=1,queued_recovered=1)
    m['rejects']=[[1,i,f,clock(f),1,error,error,164,0,0,0,0,0,224,255,*failed]
                  for i,f in enumerate((80001,90100,94100))]
    score,asset=objects(profile,voice)[1]
    m['recovered']=[150010,clock(150010),generation,1,2,0,1,0,165,0,1,0,0,3,224,255,fnv(score),fnv(asset)]
    right=[0]*m['frames']
    for index,note in enumerate(m['notes']):
        if profile not in ('both','old' if index==0 else 'new'):continue
        for i in range(note[6],note[7]):
            value=math.sin(2*math.pi*(440 if index==0 else 440*2**(3/12))*i/RATE)
            right[i]=round(2000*value if index==0 else 1500*2/math.pi*math.asin(value))
    frequency=(MASTER/5 if model=='sgb' else 4194304)/8192
    raw=b''.join(struct.pack('<hh',(1000 if math.sin(2*math.pi*frequency*i/RATE)>=0 else -1000)
                            if 60000<=i<177000 else 0,right[i]) for i in range(m['frames']))
    return m,raw


def controls(kind='asset-gap',voice=2,model='sgb'):
    return {p:observe(*synthetic(kind,p,voice,model),kind,p,voice) for p in PROFILES}


class RecoveryAudioTests(unittest.TestCase):
    def test_owned_three_payloads_and_controls(self):
        for voice in (2,3):
            for kind in FAULTS:
                for profile in PROFILES:
                    image=build(kind,profile,voice)
                    self.assertEqual(image,build(kind,profile,voice));self.assertEqual(len(image),32768)
                    self.assertEqual(image[0x4000:0x5000],pack(*objects(profile,voice)[0]))
                    self.assertEqual(image[0x5000:0x6000],replacement(kind,voice))
                    self.assertEqual(image[0x6000:0x7000],pack(*objects(profile,voice)[1]))
                    self.assertEqual(image[0x150:0x4000],build(kind,'both',voice)[0x150:0x4000])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('bad',),('asset-gap','bad'),('asset-gap','both',True),('asset-gap','both',4)):
            with self.assertRaises(ValueError):build(*args)

    def test_export_and_overwrite_refusal(self):
        with tempfile.TemporaryDirectory() as name:
            p=Path(name)/'recover.gb'
            cmd=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_recovery_audio_fixture.py'),
                 '--kind','bad-root','--voice','3','--profile','new','--output',str(p)]
            for code in (0,2):
                child=subprocess.run(cmd,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr);self.assertEqual(p.read_bytes(),build('bad-root','new',3))

    def test_both_failures_models_and_voices(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in FAULTS:
                    r=compare(controls(kind,voice,model),model,voice)
                    self.assertTrue(r['valid_retry_admitted']);self.assertTrue(r['fresh_restart_audible'])
                    self.assertEqual(r['maximum_sum_error_lsb'],dict(left=0,right=0))

    def test_stale_admission_and_missing_replay_reject(self):
        original,raw=synthetic()
        for mutation in ('mode','sounds','queued','restore','clear','clear_generation','publication','bank_hash',
                         'blocked','ready','roots','generation','error','counter','premature','late','adopt_hash','source','pitch','release','bool'):
            m=copy.deepcopy(original)
            if mutation=='mode':m['recovery_upload']=False
            elif mutation=='sounds':m['sounds']=3
            elif mutation=='queued':m['queued_recovered']=0
            elif mutation=='restore':m['restore_recovered']=0
            elif mutation=='clear':m['clears'].pop()
            elif mutation=='clear_generation':m['clears'][2][0]+=1
            elif mutation=='publication':m['banks'][1][0]=3
            elif mutation=='bank_hash':m['banks'][1][3]=m['rejects'][0][15]
            elif mutation=='blocked':m['recovered'][5]=1
            elif mutation=='ready':m['recovered'][10]=0
            elif mutation=='roots':m['recovered'][13]=2
            elif mutation=='generation':m['recovered'][2]=3
            elif mutation=='error':m['recovered'][7]=1
            elif mutation=='counter':m['recovered'][4]=3
            elif mutation=='premature':m['recovered'][0]=m['banks'][1][1]-1
            elif mutation=='late':m['recovered'][0]=m['edges'][6][2]
            elif mutation=='adopt_hash':m['recovered'][16]=m['rejects'][0][15]
            elif mutation=='source':m['notes'][1][1]=2
            elif mutation=='pitch':m['notes'][1][2]=1800
            elif mutation=='release':m['notes'][1][9]=0
            else:m['recovered'][2]=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(m,raw,'asset-gap','both',2)

    def test_stale_blocked_or_fresh_output_and_gb_disturbance_reject(self):
        meta,raw=synthetic()
        for frame in (80128,90228,94228,110128,150138,meta['edges'][7][2]+4000):
            changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
            with self.subTest(frame=frame),self.assertRaises(ValueError):observe(meta,bytes(changed),'asset-gap','both',2)
        for mutation in ('silent','late','wrong_pitch','stale','gb','timeline','pulse','blocked_pulse','sum'):
            c=controls();note=c['both']['meta']['notes'][1]
            if mutation in ('silent','late','wrong_pitch'):
                start=note[6] if mutation!='late' else note[7]-2080
                for i in range(start,note[7]):
                    value=round(2000*math.sin(2*math.pi*800*i/RATE)) if mutation=='wrong_pitch' else 0
                    c['both']['right'][i]=c['new']['right'][i]=value
            elif mutation=='stale':c['old']['right'][note[6]+500]=500;c['both']['right'][note[6]+500]+=500
            elif mutation=='gb':c['old']['left'][110500]+=1
            elif mutation=='timeline':c['new']['meta']['recovered'][0]+=1
            elif mutation in ('pulse','blocked_pulse'):
                begin=110000 if mutation=='pulse' else 90000
                for p in PROFILES:
                    for i in range(begin,begin+4000):c[p]['left'][i]=0
            else:c['both']['right'][note[6]+500]+=5
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
