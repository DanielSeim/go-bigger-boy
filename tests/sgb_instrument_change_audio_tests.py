#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject stale sources, missing return, disturbed peers and incomplete replay."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from sgb_polyphony_audio_tests import synthetic as looping_synthetic
from build_sgb_instrument_change_audio_fixture import build,objects,PROFILES
from check_sgb_instrument_change_audio import observe,compare,dual_pitch,RATE
from check_sgb_instrument_chromatic_reference import PITCHES


def synthetic(profile='both',held_voice=2):
    meta,raw=looping_synthetic('gb',held_voice)
    pcm=[list(p) for p in struct.iter_unpack('<hh',raw)]
    notes=[];completions=[]
    for voice in (2,3):
        for index in range(1 if voice==held_voice else 3):
            begin=64000+4000*index
            end=80000 if voice==held_voice else 79700 if index==2 else begin+2304
            source=5-held_voice if voice!=held_voice and index==1 else held_voice
            note=[voice,source,PITCHES[2][9 if voice==held_voice else 12],begin*2048000//RATE,
                  end*2048000//RATE,(end+96)*2048000//RATE,begin,end,end+96,0 if source!=held_voice else 63,1<<voice]
            notes.append(note)
            if profile=='gb' or (profile=='loop' and source!=held_voice) or (profile=='transient' and source==held_voice):continue
            for i in range(begin,end):
                value=(500 if (i-begin)%16<8 else -500) if source!=held_voice and 16<=i-begin<184 else (
                      0 if source!=held_voice else round(2000*math.sin(2*math.pi*(440 if voice==held_voice else 440*2**(3/12))*i/RATE)))
                pcm[i][1]+=value
    notes.sort(key=lambda n:(n[6],n[0]));mask=0
    for index,note in enumerate(notes):
        if note[1]!=held_voice:
            completions.append([note[3]+7600,note[6]+180,note[3]+7700,note[6]+182]);mask|=1<<index
        else:completions.append([0]*4)
    meta.update(schema='gbb-sgb-instrument-change-audio-v1',notes=notes,completions=completions,
                restore_releases=15,restore_zeros=15,queued_onsets=15,queued_offs=15,
                restore_ends=mask,restore_natural_zeros=mask)
    return meta,b''.join(struct.pack('<hh',*p) for p in pcm)


def controls(held=2):return {p:observe(*synthetic(p,held),p,held) for p in PROFILES}


class InstrumentChangeAudioTests(unittest.TestCase):
    def test_owned_bank_and_data_only_controls(self):
        for held in (2,3):
            original,asset=objects('both',held)
            slot=3-held;start=64+64*slot
            self.assertEqual([asset[start+9*b] for b in range(4)],[0xB0]*3+[0xB1])
            self.assertEqual(asset[19+4*slot],1)
            for profile in PROFILES:
                score,other=objects(profile,held);self.assertEqual(score,original)
                expected=bytearray(asset)
                for slot in (0,1):
                    if profile=='both' or (profile=='loop' and slot==held-2) or (profile=='transient' and slot==3-held):continue
                    base=struct.unpack_from('<H',asset,8+4*slot)[0]-0x5000
                    for b in range(asset[16+slot]):expected[base+9*b+1:base+9*b+9]=bytes(8)
                self.assertEqual(other,bytes(expected))
                stream=score[0x100+(3-held)*0x80:0x100+(3-held)*0x80+32]
                loop_id=2 if held==2 else 10;transient_id=10 if held==2 else 2
                self.assertIn(bytes((16,63,0xA4,0xE0,transient_id,0xA4,0xE0,loop_id,16,127,0xA4,0)),stream)
                image=build(profile,held)
                self.assertEqual(len(image),32768);self.assertEqual(image,build(profile,held))
                self.assertEqual(image[0x150:0x4000],build('both',held)[0x150:0x4000])
                checksum=0
                for byte in image[0x134:0x14D]:checksum=(checksum-byte-1)&255
                self.assertEqual(image[0x14D],checksum)
                self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('missing',2),('both',True),('both',4)):
            with self.assertRaises(ValueError):build(*args)

    def test_export_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'change.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_instrument_change_audio_fixture.py'),
                     '--held-voice','3','--profile','transient','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr);self.assertEqual(path.read_bytes(),build('transient',3))

    def test_both_roles_complete_and_return_to_sustained_pcm(self):
        for held in (2,3):
            result=compare(controls(held),'sgb',held)
            self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
            self.assertEqual(len(result['pitch_windows']),3)
            self.assertEqual(len(result['independent_release_windows']),2)

    def test_source_completion_and_replay_metadata_reject(self):
        original,raw=synthetic()
        for mutation in ('source','pitch','loop_completion','endx','gate','coverage','queued_on','queued_off','missing','bool','clock','release','loop_inactive','transient_active','transient_no_endx'):
            meta=copy.deepcopy(original)
            if mutation=='source':meta['notes'][3][1]=3
            elif mutation=='pitch':meta['notes'][3][2]+=100
            elif mutation=='loop_completion':meta['completions'][0]=meta['completions'][2][:]
            elif mutation=='endx':meta['completions'][2][0]=0
            elif mutation=='gate':meta['completions'][2][2]=meta['notes'][2][4]
            elif mutation=='coverage':meta['restore_ends']=0
            elif mutation=='queued_on':meta['queued_onsets']=7
            elif mutation=='queued_off':meta['queued_offs']=7
            elif mutation=='missing':meta['notes'].pop()
            elif mutation=='bool':meta['completions'][2][1]=True
            elif mutation=='clock':meta['edges'][0][3]=0
            elif mutation=='loop_inactive':meta['notes'][3][9]=0
            elif mutation=='transient_active':meta['notes'][2][9]=1
            elif mutation=='transient_no_endx':meta['notes'][2][10]=0
            else:meta['notes'][1][8]=meta['notes'][2][6]+1
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(meta,raw,'both',2)
        with self.assertRaises(ValueError):observe(original,raw[:-4],'both',2)

    def test_silent_transient_stale_tail_absent_return_and_peer_dropout_reject(self):
        for mutation in ('transient','stale','return','late_return','wrong_pitch','held','gb','timeline','sum'):
            result=controls();_,transient,returned=result['both']['peer'];begin=transient[6]
            if mutation=='transient':
                for i in range(begin,begin+500):
                    result['both']['right'][i]-=result['transient']['right'][i];result['transient']['right'][i]=0
            elif mutation in ('return','late_return','held','wrong_pitch'):
                lo,hi=(returned[6],returned[7]) if mutation in ('return','wrong_pitch') else (
                      (returned[7]-2080,returned[7]) if mutation=='late_return' else (64032,66080))
                for i in range(lo,hi):
                    held=round(2000*math.sin(2*math.pi*440*i/RATE))
                    peer=round(2000*math.sin(2*math.pi*(800 if mutation=='wrong_pitch' else 440*2**(3/12))*i/RATE))
                    value=peer if mutation=='held' else held+peer if mutation=='wrong_pitch' else held
                    result['loop']['right'][i]=value;result['both']['right'][i]=value+result['transient']['right'][i]
            elif mutation=='stale':result['transient']['right'][begin+400]=500;result['both']['right'][begin+400]+=500
            elif mutation=='gb':result['transient']['left'][begin]+=1
            elif mutation=='timeline':result['transient']['meta']['completions'][2][1]+=1
            else:result['both']['right'][begin]+=5
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(result,'sgb',2)
        result=controls();result['both']['right'][64000]+=4
        self.assertEqual(compare(result,'sgb',2)['maximum_sum_error_lsb']['right'],4)

    def test_dual_tone_detector_rejects_missing_weak_or_wrong_fundamental(self):
        for phase in (0,0.7,2.1):
            for wave in ('sine','square','triangle'):
                def signal(frequency,i):
                    value=math.sin(2*math.pi*frequency*i/RATE+phase)
                    return value if wave=='sine' else (1 if value>=0 else -1) if wave=='square' else 2/math.pi*math.asin(value)
                values=[round(2000*signal(439.5,i)+1500*signal(522.5,i)) for i in range(2048)]
                for target in (440,440*2**(3/12)):
                    self.assertLess(abs(dual_pitch(values,target)['error_cents']),10)
        target=440*2**(3/12)
        for peer_hz,amplitude in ((target,0),(target,50),(800,1500),(target*2**(20/1200),1500)):
            values=[round(2000*math.sin(2*math.pi*440*i/RATE)+amplitude*math.sin(2*math.pi*peer_hz*i/RATE)) for i in range(2048)]
            with self.subTest(hz=peer_hz,amplitude=amplitude),self.assertRaises(ValueError):dual_pitch(values,target)
        with self.assertRaises(ValueError):dual_pitch([0]*2047,target)


if __name__=='__main__':unittest.main()
