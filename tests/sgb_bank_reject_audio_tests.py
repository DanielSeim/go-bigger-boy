#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject stale/fallback PCM, readiness escapes and incomplete rejection replay."""
import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_reject_audio_fixture import build,replacement,objects,FAULTS,PROFILES
from check_sgb_bank_reject_audio import observe,compare,fnv,MASTER,RATE
spec=importlib.util.spec_from_file_location('replacement_guards',Path(__file__).with_name('sgb_bank_replace_audio_tests.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)


def synthetic(kind='asset-gap',profile='both',voice=2,model='sgb'):
    meta,raw=base.synthetic(profile,voice,model,True)
    meta['notes']=meta['notes'][:1];meta['banks']=meta['banks'][:1]
    for k in ('restore_releases','restore_zeros','queued_onsets','queued_offs','restore_banks','queued_banks'):meta[k]=1
    meta.update(rejected_upload=kind,restore_rejects=7,queued_rejects=7)
    clock=lambda frame:((frame+1)*MASTER+RATE-1)//RATE
    if kind=='asset-gap':hashes=[fnv(bytes(2048)),fnv(bytes(192))];error=1
    else:
        score,asset=objects('both',voice)[1];hashes=[fnv(bytes(2)+score[2:]),fnv(asset)];error=2
    meta['rejects']=[[1,i,frame,clock(frame),1,error,error,164,0,0,0,0,0,224,255,*hashes]
                     for i,frame in enumerate((80001,140100,155100))]
    raw=bytearray(raw)
    for frame in range(meta['notes'][0][8]+32,meta['frames']):struct.pack_into('<h',raw,frame*4+2,0)
    return meta,bytes(raw)


def controls(kind='asset-gap',voice=2,model='sgb'):
    return {p:observe(*synthetic(kind,p,voice,model),kind,p,voice) for p in PROFILES}


class RejectedBankAudioTests(unittest.TestCase):
    def test_owned_incomplete_and_semantic_payloads(self):
        for voice in (2,3):
            score,asset=objects('both',voice)[1]
            gap=replacement('asset-gap',voice);root=replacement('bad-root',voice)
            self.assertEqual(struct.unpack_from('<HH',gap,2052),(20,0x5000))
            self.assertEqual(struct.unpack_from('<HH',gap,2076),(171,0x5015))
            self.assertEqual(gap[2056:2076],asset[:20]);self.assertEqual(gap[2080:2251],asset[21:])
            self.assertEqual(root[4:2052],bytes(2)+score[2:]);self.assertEqual(root[2056:2248],asset)
            for kind in FAULTS:
                a,b=(build(kind,p,voice) for p in PROFILES)
                self.assertEqual(a,build(kind,'both',voice));self.assertEqual(len(a),32768)
                self.assertEqual(a[0x150:0x4000],b[0x150:0x4000]);self.assertEqual(a[0x5000:],b[0x5000:])
                self.assertEqual(a[0x14D],(-sum(a[0x134:0x14D])-25)&255)
                self.assertEqual(struct.unpack_from('>H',a,0x14E)[0],(sum(a)-sum(a[0x14E:0x150]))&65535)
                # Initial source controls preserve score and headers; only sample data differ.
                self.assertEqual(a[0x4000:0x4808],b[0x4000:0x4808])
        for args in (('bad',),('asset-gap','bad'),('asset-gap','both',True),('asset-gap','both',4)):
            with self.assertRaises(ValueError):build(*args)

    def test_export_and_overwrite_refusal(self):
        with tempfile.TemporaryDirectory() as name:
            p=Path(name)/'reject.gb'
            cmd=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_reject_audio_fixture.py'),
                 '--kind','bad-root','--voice','3','--profile','gb','--output',str(p)]
            for code in (0,2):
                child=subprocess.run(cmd,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr);self.assertEqual(p.read_bytes(),build('bad-root','gb',3))

    def test_both_failures_models_and_voices(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in FAULTS:
                    result=compare(controls(kind,voice,model),model,voice)
                    self.assertTrue(result['blocked_sound_silent']);self.assertTrue(result['uninterrupted_gb_equal'])

    def test_missing_rejection_replay_and_stale_native_data_reject(self):
        original,raw=synthetic()
        for mutation in ('identity','sound','replay','queued','clear','note','bank','ready','blocked','error',
                         'generation','phase','hash','count','early','mute','gaps','gap_stage','gap_after','gap_size','clock','bool'):
            m=copy.deepcopy(original)
            if mutation=='identity':m['rejected_upload']='bad-root'
            elif mutation=='sound':m['sounds']=1
            elif mutation=='replay':m['restore_rejects']=3
            elif mutation=='queued':m['queued_rejects']=3
            elif mutation=='clear':m['clears'].pop()
            elif mutation=='note':m['notes'].append(m['notes'][0])
            elif mutation=='bank':m['banks'].append(m['banks'][0])
            elif mutation=='ready':m['rejects'][0][9]=1
            elif mutation=='blocked':m['rejects'][0][4]=0
            elif mutation=='error':m['rejects'][0][5]=2
            elif mutation=='generation':m['rejects'][0][6]=2
            elif mutation=='phase':m['rejects'][0][7]=165
            elif mutation=='hash':m['rejects'][0][15]+=1
            elif mutation=='count':m['rejects'][1][1]=0
            elif mutation=='early':m['rejects'][1][2]=m['edges'][3][2]-1
            elif mutation=='mute':m['mute'][4]=0
            elif mutation=='gaps':m['gaps']=[]
            elif mutation=='gap_stage':m['gaps'][0][4]=2
            elif mutation=='gap_after':m['gaps'][0][5]=1
            elif mutation=='gap_size':m['gaps'][0][3]+=512
            elif mutation=='clock':m['rejects'][2][3]=0
            else:m['rejects'][0][0]=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(m,raw,'asset-gap','both',2)
        with self.assertRaises(ValueError):observe(original,raw[:-4],'asset-gap','both',2)

    def test_resurrected_audio_silent_old_tone_and_gb_disturbance_reject(self):
        meta,raw=synthetic()
        for frame in (meta['notes'][0][8]+64,meta['rejects'][0][2]+128,meta['rejects'][1][2]+128,meta['rejects'][2][2]+128):
            changed=bytearray(raw);struct.pack_into('<h',changed,frame*4+2,500)
            with self.subTest(frame=frame),self.assertRaises(ValueError):observe(meta,bytes(changed),'asset-gap','both',2)
        for mutation in ('silent','late','gb','timeline'):
            c=controls()
            if mutation in ('silent','late'):
                start=0 if mutation=='silent' else 76000
                for i in range(start,79000):c['both']['right'][i]=0
            elif mutation=='gb':c['gb']['left'][90000]+=1
            else:c['gb']['meta']['rejects'][1][2]+=1
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
