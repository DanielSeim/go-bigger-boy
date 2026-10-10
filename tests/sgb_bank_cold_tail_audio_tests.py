#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cold one-byte token synchronization, repeated rejection and first audio guards."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from check_sgb_bank_cold_tail_audio import observe,compare,fixture,CASES,PROFILES
from check_sgb_bank_cold_audio import MASTER,RATE
from build_sgb_bank_cold_audio_fixture import build,objects,semantic_payload
from sgb_bank_cold_audio_tests import synthetic as cold_synthetic


def synthetic(kind='tail',profile='both',voice=2,model='sgb'):
    m,raw=cold_synthetic('bad-root',profile,voice,model)
    repeat=kind=='repeat-tail'
    m.update(semantic_tail=True,repeated_rejection=repeat,consumed_tokens=[3]*(5 if repeat else 3))
    if not repeat:return m,raw
    clock=lambda frame:((frame+1)*MASTER+RATE-1)//RATE
    positions=(60000,73000,76000,83000,113000,120000,170000,180000)
    m.update(sounds=5,restore_edges=255,restore_clears=7,queued_clears=7,restore_rejects=31,queued_rejects=31)
    m['edges']=[[i+1,0 if i==7 or profile=='native' else 16,f,clock(f)] for i,f in enumerate(positions)]
    m['notes'][0][3:9]=[*[f*2048000//RATE for f in (174000,180100,180200)],174000,180100,180200]
    m['banks'][0][:3]=[3,136000,clock(136000)]
    m['clears']=[[0,12400,clock(12400)],[1,85000,clock(85000)],[2,122000,clock(122000)]]
    for rejects,suppressed,frame in ((2,2,100000),(2,3,113100)):
        e=m['rejects'][0].copy();e[:4]=[rejects,suppressed,frame,clock(frame)];e[6]=2;m['rejects'].append(e)
    m['recovered'][:5]=[136010,clock(136010),3,2,3]
    frequency=(MASTER/5 if model=='sgb' else 4194304)/8192
    raw=b''.join(struct.pack('<hh',
        (1000 if math.sin(2*math.pi*frequency*i/RATE)>=0 else -1000) if profile!='native' and 60000<=i<180000 else 0,
        round(1500*2/math.pi*math.asin(math.sin(2*math.pi*440*2**(3/12)*i/RATE))) if profile!='gb' and 174000<=i<180100 else 0)
        for i in range(m['frames']))
    return m,raw


def controls(kind='tail',voice=2,model='sgb'):
    return {p:observe(*synthetic(kind,p,voice,model),kind,p,voice) for p in PROFILES}


class ColdTailAudioTests(unittest.TestCase):
    def test_physical_one_byte_tail_and_repeated_commands(self):
        for voice in (2,3):
            score,asset=objects('both',voice)[1]
            score=bytes(2)+score[2:]
            expected=(struct.pack('<HH',2048,0x2B00)+score+struct.pack('<HH',191,0x5000)+asset[:191]+
                      struct.pack('<HH',1,0x50BF)+asset[191:]+struct.pack('<HH',0,0x0400))
            for kind in CASES:
                for profile in PROFILES:
                    image=fixture(kind,profile,voice)
                    self.assertEqual(image,fixture(kind,profile,voice));self.assertEqual(len(image),32768)
                    self.assertEqual(image[0x4000:0x5000],expected+bytes(4096-len(expected)))
                    self.assertEqual(image[0x4000:0x5000],semantic_payload(voice))
                    self.assertEqual(image[0x5000:],build('bad-root',profile,voice)[0x5000:])
                    both=fixture(kind,'both',voice)
                    differences=[(a,b) for a,b in zip(both[0x150:0x4000],image[0x150:0x4000]) if a!=b]
                    self.assertEqual(differences,[(16,0)] if profile=='native' else [])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for kwargs in (dict(semantic_tail=True),dict(repeat=True),dict(semantic_tail=1),dict(repeat=1)):
            with self.assertRaises(ValueError):build(**kwargs)
        with self.assertRaises(ValueError):fixture('bad','both',2)
        with tempfile.TemporaryDirectory() as name:
            output=Path(name)/'cold.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_cold_audio_fixture.py'),'--kind','bad-root',
                     '--semantic-tail','--repeat','--voice','3','--profile','native','--output',str(output)]
            for expected_code in (0,2):
                result=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,expected_code,result.stderr)
                self.assertEqual(output.read_bytes(),fixture('repeat-tail','native',3))

    def test_source_comparisons_for_both_models_voices_cases(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in CASES:
                    result=compare(controls(kind,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(len(result['gb_pulse_windows']),7 if kind=='repeat-tail' else 5)

    def test_missing_token_replay_and_stale_second_rejection(self):
        for kind in CASES:
            original,raw=synthetic(kind)
            for mutation in ('identity','repeat_identity','missing','extra','wrong','bool','queued_clear','queued_failure',
                             'generation','blocked','error','readiness','ram','admission','clear','early_note'):
                m=copy.deepcopy(original)
                if mutation=='identity':m['semantic_tail']=False
                elif mutation=='repeat_identity':m['repeated_rejection']=not m['repeated_rejection']
                elif mutation=='missing':m['consumed_tokens'].pop()
                elif mutation=='extra':m['consumed_tokens'].append(3)
                elif mutation=='wrong':m['consumed_tokens'][-1]=7
                elif mutation=='bool':m['consumed_tokens'][0]=True
                elif mutation=='queued_clear':m['queued_clears']&=~2
                elif mutation=='queued_failure':m['queued_rejects']&=~(16 if kind=='repeat-tail' else 4)
                elif mutation=='generation':m['rejects'][-1][6]-=1
                elif mutation=='blocked':m['rejects'][-1][4]=0
                elif mutation=='error':m['rejects'][-1][5]=0
                elif mutation=='readiness':m['rejects'][-1][9]=1
                elif mutation=='ram':m['rejects'][-1][16]=0
                elif mutation=='admission':m['recovered'][3]=0
                elif mutation=='clear':m['clears'].pop()
                else:m['notes'][0][6]=98000
                with self.subTest(kind=kind,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,kind,'both',2)
        m,raw=synthetic('repeat-tail')
        for index in (3,4):
            for field in (0,1,6):
                changed=copy.deepcopy(m);changed['rejects'][index][field]-=1
                with self.subTest(index=index,field=field),self.assertRaises(ValueError):observe(changed,raw,'repeat-tail','both',2)

    def test_early_native_audio_and_second_failure_gb_dropout(self):
        m,raw=synthetic('repeat-tail')
        for frame in (1000,20000,85128,100128,113228,122128,136138,173999,184000):
            changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
            with self.subTest(frame=frame),self.assertRaises(ValueError):observe(m,bytes(changed),'repeat-tail','both',2)
        for begin in (85000,113100,122000):
            c=controls('repeat-tail')
            for profile in ('both','gb'):c[profile]['left'][begin:begin+4000]=[0]*4000
            with self.subTest(begin=begin),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
