#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public source-isolation, overlap, release and PCM sum guards."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_polyphony_audio_fixture import build,objects,PROFILES,PEER_NOTES
from check_sgb_polyphony_audio import observe,compare,RATE,MASTER
from check_sgb_instrument_chromatic_reference import PITCHES


def synthetic(profile='both',held_voice=2):
    frames=122920
    positions=(60000,110000)
    edges=[[i+1,16 if i==0 else 0,f,((f+1)*MASTER+RATE-1)//RATE] for i,f in enumerate(positions)]
    notes=[]
    for voice in (2,3):
        pitches=(33,) if voice==held_voice else PEER_NOTES
        for index,note in enumerate(pitches):
            begin=64000+4000*index
            end=80000 if voice==held_voice else begin+2304
            notes.append([voice,voice,PITCHES[2][note-24],begin*2048000//RATE,end*2048000//RATE,
                          (end+96)*2048000//RATE,begin,end,end+96])
    notes.sort(key=lambda n:(n[6],n[0]))
    meta=dict(schema='gbb-sgb-polyphony-audio-v1',qualification=False,playback=False,
              reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=2,version=0xDA,
              restore_edges=3,restore_releases=31,restore_zeros=31,pending_restores=20,
              frames=frames,clocks=55000028,restore_count=314,gb_samples=115062,native_samples=81947,
              edges=edges,notes=notes)
    sources={2:[0]*frames,3:[0]*frames}
    for voice,slot,register,on,off,zero,begin,end,quiet in notes:
        index=next(i for i,p in enumerate(PITCHES[2]) if p==register)
        hz=440*2**((index-9)/12)
        for i in range(begin,end):sources[voice][i]=round(2000*math.sin(2*math.pi*hz*i/RATE))
    pcm=[]
    for i in range(frames):
        snes=sum(sources[v][i] for v in (2,3) if profile=='both' or profile==f'voice{v}')
        gb=(1000 if math.sin(2*math.pi*(MASTER/5/8192)*i/RATE)>=0 else -1000) if positions[0]<=i<positions[1] else 0
        pcm.append((gb,snes))
    return meta,b''.join(struct.pack('<hh',*sample) for sample in pcm)


def controls(held_voice=2):
    results={}
    for profile in PROFILES:
        meta,raw=synthetic(profile,held_voice)
        results[profile]=observe(meta,raw,profile,held_voice)
    return results


class PolyphonyAudioTests(unittest.TestCase):
    def test_owned_source_controls_preserve_score_and_brr_headers(self):
        for held in (2,3):
            score,original=objects('both',held)
            for profile in PROFILES:
                other,asset=objects(profile,held)
                self.assertEqual(other,score)
                expected=bytearray(original)
                for voice in (2,3):
                    start=struct.unpack_from('<H',original,8+4*(voice-2))[0]-0x5000
                    if profile!='both' and profile!=f'voice{voice}':
                        for block in (0,1):expected[start+9*block+1:start+9*block+9]=bytes(8)
                self.assertEqual(asset,bytes(expected))
                image=build(profile,held)
                self.assertEqual(len(image),32768)
                self.assertEqual(image,build(profile,held))
                self.assertEqual(image[0x150:0x4000],build('both',held)[0x150:0x4000])
                self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('missing',2),('both',True),('both',4)):
            with self.assertRaises(ValueError):build(*args)

    def test_export_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'polyphony.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_polyphony_audio_fixture.py'),
                     '--held-voice','3','--profile','voice2','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr)
                self.assertEqual(path.read_bytes(),build('voice2',3))

    def test_both_held_voice_paths_and_independent_release(self):
        for held in (2,3):
            result=compare(controls(held),'sgb',held)
            self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
            self.assertEqual(len(result['independent_release_windows']),3)
            self.assertEqual(len(result['pitch_windows']),4)

    def test_sum_boundary_and_missing_or_duplicated_source_reject(self):
        result=controls()
        result['both']['right'][65000]+=4
        self.assertEqual(compare(result,'sgb',2)['maximum_sum_error_lsb']['right'],4)
        result['both']['right'][65000]+=1
        with self.assertRaises(ValueError):compare(result,'sgb',2)
        for mutation in ('missing','duplicated','gb_phase','stale_peer','quiet_held'):
            result=controls()
            if mutation=='missing':result['both']['right']=result['voice2']['right'][:]
            elif mutation=='duplicated':result['both']['right']=[a+b for a,b in zip(result['both']['right'],result['voice3']['right'])]
            elif mutation=='gb_phase':result['voice2']['left'][65000]+=1000
            else:
                i=result['both']['peer'][0][8]+8
                if mutation=='stale_peer':
                    result['voice3']['right'][i]=500;result['both']['right'][i]+=500
                else:
                    result['voice2']['right'][i]=0;result['both']['right'][i]=0
                    # Silence the entire peer gap while keeping the sum exact.
                    a,b=result['both']['peer'][0][8]+4,result['both']['peer'][1][6]-4
                    result['voice2']['right'][a:b]=[0]*(b-a);result['both']['right'][a:b]=[0]*(b-a)
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(result,'sgb',2)

    def test_wrong_pitch_rejects_even_if_sum_still_matches(self):
        result=controls()
        note=result['both']['peer'][0];begin=note[6]+32
        for j in range(2048):
            i=begin+j;value=round(2000*math.sin(2*math.pi*800*j/RATE))
            result['voice3']['right'][i]=value
            result['both']['right'][i]=result['voice2']['right'][i]+value
        with self.assertRaises(ValueError):compare(result,'sgb',2)

    def test_lifecycle_coverage_clock_and_note_mutations_reject(self):
        original,raw=synthetic()
        variants=[dict(original,pending_restores=0),dict(original,restore_releases=15),
                  dict(original,restore_zeros=15),dict(original,restore_equal=False),dict(original,frames=1)]
        for mutation in ('voice','pitch','release','zero','clock','bool','missing'):
            meta=copy.deepcopy(original)
            if mutation=='voice':meta['notes'][0][0]=4
            elif mutation=='pitch':meta['notes'][1][2]=1800
            elif mutation=='release':meta['notes'][0][7]=meta['notes'][1][6]+1
            elif mutation=='zero':meta['notes'][1][8]=meta['notes'][2][6]+1
            elif mutation=='clock':meta['edges'][0][3]=0
            elif mutation=='bool':meta['notes'][0][6]=True
            else:meta['notes'].pop()
            variants.append(meta)
        for i,meta in enumerate(variants):
            with self.subTest(case=i),self.assertRaises(ValueError):observe(meta,raw,'both',2)
        with self.assertRaises(ValueError):observe(original,raw[:-4],'both',2)
        changed=bytearray(raw);struct.pack_into('<h',changed,4*(110000+4000),100)
        with self.assertRaises(ValueError):observe(original,bytes(changed),'both',2)


if __name__=='__main__':unittest.main()
