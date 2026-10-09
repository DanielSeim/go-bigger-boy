#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public natural completion and independently audible retrigger rejection guards."""
import copy
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from sgb_polyphony_audio_tests import synthetic as looping_synthetic
from build_sgb_one_shot_audio_fixture import build,objects,PROFILES
from check_sgb_one_shot_audio import observe,compare


def synthetic(profile='both',held_voice=2):
    meta,raw=looping_synthetic('gb',held_voice)
    _,held_raw=looping_synthetic(f'voice{held_voice}',held_voice)
    pcm=[list(x) for x in struct.iter_unpack('<hh',held_raw if profile in ('both',f'voice{held_voice}') else raw)]
    completions=[];mask=0
    for index,note in enumerate(meta['notes']):
        if note[0]==held_voice:
            completions.append([0]*4);continue
        mask|=1<<index;begin=note[6]
        completions.append([note[3]+7600,begin+180,note[3]+7700,begin+182])
        if profile in ('both',f'voice{5-held_voice}'):
            for i in range(16,184):pcm[begin+i][1]+=500 if i%16<8 else -500
    meta.update(completions=completions,restore_ends=mask,restore_natural_zeros=mask)
    return meta,b''.join(struct.pack('<hh',*x) for x in pcm)


def controls(held=2):
    return {p:observe(*synthetic(p,held),p,held) for p in PROFILES}


class OneShotAudioTests(unittest.TestCase):
    def test_owned_geometry_and_matched_data_only_controls(self):
        for held in (2,3):
            score,original=objects('both',held)
            slot=3-held;base=64+64*slot
            self.assertEqual(original[16+slot],4)
            self.assertEqual(original[19+4*slot],1)
            self.assertEqual(struct.unpack_from('<HH',original,8+4*slot),(0x5000+base,)*2)
            self.assertEqual([original[base+9*b] for b in range(4)],[0xB0]*3+[0xB1])
            self.assertEqual(original[base+36:base+64],bytes(28))
            for profile in PROFILES:
                other,asset=objects(profile,held);self.assertEqual(other,score)
                expected=bytearray(original)
                for voice in (2,3):
                    if profile=='both' or profile==f'voice{voice}':continue
                    slot=voice-2;start=struct.unpack_from('<H',original,8+4*slot)[0]-0x5000
                    for b in range(original[16+slot]):expected[start+9*b+1:start+9*b+9]=bytes(8)
                self.assertEqual(asset,bytes(expected))
                image=build(profile,held);self.assertEqual(image,build(profile,held));self.assertEqual(len(image),32768)
                self.assertEqual(image[0x150:0x4000],build('both',held)[0x150:0x4000])
                self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for args in (('missing',2),('both',True),('both',4)):
            with self.assertRaises(ValueError):build(*args)

    def test_export_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'one-shot.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_one_shot_audio_fixture.py'),
                     '--held-voice','3','--profile','voice2','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr);self.assertEqual(path.read_bytes(),build('voice2',3))

    def test_both_roles_natural_completion_and_retrigger(self):
        for held in (2,3):
            result=compare(controls(held),'sgb',held)
            self.assertEqual(len(result['transients']),4)
            self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))

    def test_false_natural_completion_and_missing_restore_reject(self):
        original,raw=synthetic()
        for mutation in ('endx','gate','loop','missing','boolean','coverage','delayed'):
            meta=copy.deepcopy(original)
            peer=next(i for i,n in enumerate(meta['notes']) if n[0]==3)
            c=meta['completions'][peer];note=meta['notes'][peer]
            if mutation=='endx':c[0]=0
            elif mutation=='gate':c[2]=note[4];c[3]=note[7]
            elif mutation=='loop':meta['completions'][0]=c[:]
            elif mutation=='missing':meta['completions'].pop()
            elif mutation=='boolean':c[1]=True
            elif mutation=='coverage':meta['restore_ends']=0
            else:c[3]=note[6]+1000
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(meta,raw,'both',2)

    def test_silent_retrigger_stale_tail_and_held_dropout_reject_with_correct_sum(self):
        for mutation in ('silent','stale','held','gb','timeline','sum'):
            result=controls();note=result['both']['peer'][1];begin=note[6]
            if mutation=='silent':
                for i in range(begin,begin+500):
                    result['both']['right'][i]-=result['voice3']['right'][i];result['voice3']['right'][i]=0
            elif mutation=='stale':
                result['voice3']['right'][begin+400]=500;result['both']['right'][begin+400]+=500
            elif mutation=='held':
                for i in range(begin+32,begin+2080):
                    result['both']['right'][i]-=result['voice2']['right'][i];result['voice2']['right'][i]=0
            elif mutation=='gb':result['voice3']['left'][begin]+=1
            elif mutation=='timeline':result['voice3']['meta']['completions'][1][1]+=1
            else:result['both']['right'][begin]+=5
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(result,'sgb',2)
        result=controls();result['both']['right'][64000]+=4
        self.assertEqual(compare(result,'sgb',2)['maximum_sum_error_lsb']['right'],4)


if __name__=='__main__':unittest.main()
