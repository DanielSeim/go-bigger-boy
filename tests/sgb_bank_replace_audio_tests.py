#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject stale banks, missing fresh PCM and incomplete replacement replay."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_replace_audio_fixture import build,objects,PROFILES
from build_sgb_owned_sample_fixture import calibrated_assets
from check_sgb_bank_replace_audio import observe,compare,RATE,MASTER,fnv
from check_sgb_instrument_chromatic_reference import PITCHES


def synthetic(profile='both',voice=2,model='sgb'):
    clocks=100000016;frames=clocks*RATE//MASTER
    clock=lambda frame:((frame+1)*MASTER+RATE-1)//RATE
    positions=(60000,70000,75000,140000,155000)
    edges=[[i+1,0 if i==4 else 16,f,clock(f)] for i,f in enumerate(positions)]
    notes=[]
    for index,(begin,end) in enumerate(((64000,70100),(144000,155100))):
        notes.append([voice,index+2,PITCHES[2][9 if index==0 else 12],begin*2048000//RATE,
                      end*2048000//RATE,(end+100)*2048000//RATE,begin,end,end+100,63,1<<voice])
    banks=[];clears=[]
    for index,((score,asset),cleared,published) in enumerate(zip(objects(profile,voice),(1000,80000),(5000,130000))):
        banks.append([index+1,published,clock(published),fnv(score),fnv(asset),*asset[24:26]])
        clears.append([index,cleared,clock(cleared)])
    meta=dict(schema='gbb-sgb-bank-replace-audio-v1',qualification=False,playback=False,
              reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=4,version=0xDA,
              restore_edges=31,restore_releases=3,restore_zeros=3,queued_onsets=3,queued_offs=3,
              restore_banks=3,restore_clears=3,queued_banks=3,queued_clears=3,
              frames=frames,clocks=clocks,restore_count=800,pending_restores=200,
              gb_samples=200000,native_samples=150000,edges=edges,notes=notes,banks=banks,clears=clears)
    right=[0]*frames
    for index,note in enumerate(notes):
        if profile not in ('both','old' if index==0 else 'new'):continue
        for i in range(note[6],note[7]):
            value=math.sin(2*math.pi*(440 if index==0 else 440*2**(3/12))*i/RATE)
            right[i]=round(2000*value if index==0 else 1500*2/math.pi*math.asin(value))
    frequency=(MASTER/5 if model=='sgb' else 4194304)/8192
    return meta,b''.join(struct.pack('<hh',
        (1000 if math.sin(2*math.pi*frequency*i/RATE)>=0 else -1000) if positions[0]<=i<positions[4] else 0,
        right[i]) for i in range(frames))


def controls(voice=2,model='sgb'):return {p:observe(*synthetic(p,voice,model),p,voice) for p in PROFILES}


class BankReplaceAudioTests(unittest.TestCase):
    def test_owned_map_waveform_and_data_only_controls(self):
        reference=calibrated_assets();triangle=struct.unpack_from('<H',reference,12)[0]-0x5000
        for voice in (2,3):
            full=objects('both',voice)
            for score,_ in full:
                for channel in (2,3):
                    offset=struct.unpack_from('<H',score,0x60+2*channel)[0]-0x2B00
                    self.assertEqual(score[score.index(0xE1,offset,offset+32)+1],0)
            self.assertEqual([a[24:26] for _,a in full],[bytes((2,10)),bytes((10,2))])
            new=full[1][1];start=struct.unpack_from('<H',new,12)[0]-0x5000
            self.assertEqual(new[start:start+18],reference[triangle:triangle+18])
            old=full[0][1];start_old=struct.unpack_from('<H',old,8)[0]-0x5000
            self.assertNotEqual(old[start_old:start_old+18],new[start:start+18])
            for profile in PROFILES:
                for bank,((score,asset),(original,data)) in enumerate(zip(objects(profile,voice),full)):
                    self.assertEqual(score,original);expected=bytearray(data)
                    if profile=='gb' or (profile=='old' and bank==1) or (profile=='new' and bank==0):
                        for slot in (0,1):
                            base=struct.unpack_from('<H',data,8+4*slot)[0]-0x5000
                            for block in range(data[16+slot]):expected[base+9*block+1:base+9*block+9]=bytes(8)
                    self.assertEqual(asset,bytes(expected))
                image=build(profile,voice);self.assertEqual(image,build(profile,voice));self.assertEqual(len(image),32768)
                self.assertEqual(image[0x150:0x4000],build('both',voice)[0x150:0x4000])
                self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('missing',2),('both',True),('both',4)):
            with self.assertRaises(ValueError):build(*args)

    def test_export_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'bank.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_replace_audio_fixture.py'),
                     '--voice','3','--profile','new','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr);self.assertEqual(path.read_bytes(),build('new',3))

    def test_both_models_and_voices_fresh_bank_and_gb(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                result=compare(controls(voice,model),model,voice)
                self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                self.assertEqual(len(result['pitch_windows']),2)

    def test_stale_metadata_missing_clear_and_replay_coverage_reject(self):
        original,raw=synthetic()
        for mutation in ('source','mapping','hash','missing_clear','clear_generation','publication','queued_bank','queued_clear','release','inactive','restore','bool','clock'):
            meta=copy.deepcopy(original)
            if mutation=='source':meta['notes'][1][1]=2
            elif mutation=='mapping':meta['banks'][1][5:]=meta['banks'][0][5:]
            elif mutation=='hash':meta['banks'][1][4]=meta['banks'][0][4]
            elif mutation=='missing_clear':meta['clears'].pop()
            elif mutation=='clear_generation':meta['clears'][1][0]=0
            elif mutation=='publication':meta['banks'][1][1]=meta['edges'][3][2]
            elif mutation=='queued_bank':meta['queued_banks']=1
            elif mutation=='queued_clear':meta['queued_clears']=1
            elif mutation=='release':meta['notes'][0][8]=meta['edges'][2][2]
            elif mutation=='inactive':meta['notes'][0][9]=0
            elif mutation=='restore':meta['restore_equal']=False
            elif mutation=='bool':meta['banks'][1][0]=True
            else:meta['clears'][1][2]=0
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(meta,raw,'both',2)
        with self.assertRaises(ValueError):observe(original,raw[:-4],'both',2)
        changed=bytearray(raw);struct.pack_into('<h',changed,4*(original['edges'][4][2]+4000),100)
        with self.assertRaises(ValueError):observe(original,bytes(changed),'both',2)

    def test_missing_new_audio_stale_old_tail_wrong_pitch_and_gb_disturbance_reject(self):
        for mutation in ('silent','late','wrong_pitch','stale','gap','gb','timeline','sum'):
            result=controls();note=result['both']['meta']['notes'][1]
            if mutation in ('silent','late','wrong_pitch'):
                begin=note[6] if mutation!='late' else note[7]-2080
                for i in range(begin,note[7]):
                    value=round(2000*math.sin(2*math.pi*800*i/RATE)) if mutation=='wrong_pitch' else 0
                    result['new']['right'][i]=value;result['both']['right'][i]=value
            elif mutation in ('stale','gap'):
                i=note[6]+500 if mutation=='stale' else 90000
                result['old']['right'][i]=500;result['both']['right'][i]+=500
            elif mutation=='gb':result['new']['left'][90000]+=1
            elif mutation=='timeline':result['new']['meta']['clears'][1][1]+=1
            else:result['both']['right'][note[6]+500]+=5
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(result,'sgb',2)


if __name__=='__main__':unittest.main()
