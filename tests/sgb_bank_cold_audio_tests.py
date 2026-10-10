#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject premature first-bank audio, stale cold admission and missing replay."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_cold_audio_fixture import build,pack,objects,replacement,FAULTS,PROFILES
from check_sgb_bank_cold_audio import observe,compare,fnv,MASTER,RATE


def synthetic(kind='asset-gap',profile='both',voice=2,model='sgb'):
    clock=lambda frame:((frame+1)*MASTER+RATE-1)//RATE
    error=1 if kind=='asset-gap' else 2
    frames=100000000*RATE//MASTER
    score,asset=objects('gb' if profile=='gb' else 'both',voice)[1];fresh=[fnv(score),fnv(asset)]
    score,asset=objects('both',voice)[1]
    failed=[fnv(bytes(2048)),fnv(bytes(192))] if kind=='asset-gap' else [fnv(bytes(2)+score[2:]),fnv(asset)]
    positions=(60000,73000,76000,83000,133000,143000)
    m=dict(schema='gbb-sgb-bank-replace-audio-v1',qualification=False,playback=False,cold_rejection=True,
        recovery_upload=True,rejected_upload=kind,reset_equal=True,restore_equal=True,sample_rate_hz=RATE,
        clipped=0,sounds=4,version=0xDA,restore_edges=63,restore_releases=1,restore_zeros=1,
        queued_onsets=1,queued_offs=1,restore_banks=1,queued_banks=1,restore_clears=3,queued_clears=3,
        restore_rejects=7,queued_rejects=7,restore_recovered=1,queued_recovered=1,
        clocks=100000000,frames=frames,restore_count=900,pending_restores=450,gb_samples=frames,native_samples=200000,
        edges=[[i+1,0 if i==5 or profile=='native' else 16,f,clock(f)] for i,f in enumerate(positions)],
        notes=[[voice,3,2140,*[f*2048000//RATE for f in (137000,143100,143200)],137000,143100,143200,97,1<<voice]],
        banks=[[error,99000,clock(99000),*fresh,10,2]],
        clears=[[0,12400,clock(12400)],[error-1,85000,clock(85000)]],
        rejects=[[1,i,f,clock(f),1,error,error-1,164,0,0,0,0,0,224,255,*failed] for i,f in enumerate((20000,73100,76100))],
        recovered=[99010,clock(99010),error,1,2,0,1,0,165,0,1,0,0,3,224,255,*fresh])
    frequency=(MASTER/5 if model=='sgb' else 4194304)/8192
    raw=b''.join(struct.pack('<hh',
        (1000 if math.sin(2*math.pi*frequency*i/RATE)>=0 else -1000) if profile!='native' and 60000<=i<143000 else 0,
        round(1500*2/math.pi*math.asin(math.sin(2*math.pi*440*2**(3/12)*i/RATE))) if profile!='gb' and 137000<=i<143100 else 0)
        for i in range(frames))
    return m,raw


def controls(kind='asset-gap',voice=2,model='sgb'):
    return {p:observe(*synthetic(kind,p,voice,model),kind,p,voice) for p in PROFILES}


class ColdAudioTests(unittest.TestCase):
    def test_owned_cold_payloads_and_source_controls(self):
        for kind in FAULTS:
            for voice in (2,3):
                for profile in PROFILES:
                    image=build(kind,profile,voice)
                    self.assertEqual(image,build(kind,profile,voice));self.assertEqual(len(image),32768)
                    self.assertEqual(image[0x4000:0x5000],replacement(kind,voice))
                    self.assertEqual(image[0x5000:0x6000],pack(*objects('gb' if profile=='gb' else 'both',voice)[1]))
                    self.assertFalse(any(image[0x6000:]))
                    other=build(kind,'both',voice)
                    if profile=='native':
                        changes=[(a,b) for a,b in zip(other[0x150:0x4000],image[0x150:0x4000]) if a!=b]
                        self.assertEqual(changes,[(16,0)])
                    else:self.assertEqual(image[0x150:0x4000],other[0x150:0x4000])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('bad',),('asset-gap','bad'),('asset-gap','both',True),('asset-gap','both',4)):
            with self.assertRaises(ValueError):build(*args)
        with tempfile.TemporaryDirectory() as name:
            p=Path(name)/'cold.gb';command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_cold_audio_fixture.py'),
                '--kind','bad-root','--profile','native','--voice','3','--output',str(p)]
            for code in (0,2):
                r=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(r.returncode,code,r.stderr);self.assertEqual(p.read_bytes(),build('bad-root','native',3))

    def test_both_failures_models_and_voices(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in FAULTS:
                    result=compare(controls(kind,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(len(result['gb_pulse_windows']),5)
                    self.assertTrue(result['first_bank_admitted'])

    def test_premature_note_publication_and_stale_admission_rejected(self):
        for kind in FAULTS:
            original,raw=synthetic(kind)
            for mutation in ('mode','sounds','queued','note_count','source','pitch','early_note','release','edge',
                             'bank_count','bank_generation','bank_hash','bank_time','clear_count','clear_generation',
                             'first_clear','failure','error','handoff','blocked','ready','failed_hash','suppression_time',
                             'admit_count','admit_error','admit_generation','admit_blocked','admit_hash','admit_time','bool'):
                m=copy.deepcopy(original)
                if mutation=='mode':m['cold_rejection']=False
                elif mutation=='sounds':m['sounds']=5
                elif mutation=='queued':m['queued_recovered']=0
                elif mutation=='note_count':m['notes'].append(m['notes'][0].copy())
                elif mutation=='source':m['notes'][0][1]=2
                elif mutation=='pitch':m['notes'][0][2]=1800
                elif mutation=='early_note':m['notes'][0][6]=98000
                elif mutation=='release':m['notes'][0][9]=0
                elif mutation=='edge':m['restore_edges']=31
                elif mutation=='bank_count':m['banks'].append(m['banks'][0].copy())
                elif mutation=='bank_generation':m['banks'][0][0]+=1
                elif mutation=='bank_hash':m['banks'][0][3]=0
                elif mutation=='bank_time':m['banks'][0][1]=82000
                elif mutation=='clear_count':m['clears'].pop()
                elif mutation=='clear_generation':m['clears'][1][0]+=1
                elif mutation=='first_clear':m['clears'][0][0]=1
                elif mutation=='failure':m['rejects'].pop()
                elif mutation=='error':m['rejects'][0][5]=3
                elif mutation=='handoff':m['rejects'][0][6]+=1
                elif mutation=='blocked':m['rejects'][0][4]=0
                elif mutation=='ready':m['rejects'][0][9]=1
                elif mutation=='failed_hash':m['rejects'][0][15]=0
                elif mutation=='suppression_time':m['rejects'][1][2]=72000
                elif mutation=='admit_count':m['recovered'][3]=2
                elif mutation=='admit_error':m['recovered'][7]=1
                elif mutation=='admit_generation':m['recovered'][2]+=1
                elif mutation=='admit_blocked':m['recovered'][5]=1
                elif mutation=='admit_hash':m['recovered'][16]=0
                elif mutation=='admit_time':m['recovered'][0]=98000
                else:m['recovered'][2]=True
                with self.subTest(kind=kind,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,kind,'both',2)

    def test_native_leakage_source_mix_and_common_gb_dropout_rejected(self):
        m,raw=synthetic()
        for frame in (1000,20000,73228,76228,85128,99138,136999,147000):
            changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
            with self.subTest(frame=frame),self.assertRaises(ValueError):observe(m,bytes(changed),'asset-gap','both',2)
        for mutation in ('native_left','gb_right','sum','timeline','silent','wrong_pitch','brief','initial_gb','blocked_gb','retry_gb'):
            c=controls()
            if mutation=='native_left':c['native']['left'][137500]=1
            elif mutation=='gb_right':c['gb']['right'][137500]=1
            elif mutation=='sum':c['both']['right'][137500]+=5
            elif mutation=='timeline':c['native']['meta']['recovered'][0]+=1
            elif mutation in ('silent','wrong_pitch','brief'):
                for i in range(137000,143100):
                    value=round(1500*math.sin(2*math.pi*800*i/RATE)) if mutation=='wrong_pitch' else 0
                    if mutation!='brief' or i<142000:c['both']['right'][i]=c['native']['right'][i]=value
            else:
                begin=60000 if mutation=='initial_gb' else 73100 if mutation=='blocked_gb' else 85000
                for p in ('both','gb'):c[p]['left'][begin:begin+4000]=[0]*4000
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
