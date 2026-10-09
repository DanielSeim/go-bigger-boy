#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned transition fixture and PCM-dependent continuity guards."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_audio_transition_fixture import build,objects,PROFILES,ROUTES
from build_sgb_score_atomic_fixture import build_cartridge
from check_sgb_audio_transitions import observe,compare,RATE,MASTER


def synthetic(profile='toggle'):
    frames=122920
    positions=(60000,66400,69600,72800,77600,87200)
    edges=[[i+1,route if profile=='toggle' else 16 if profile=='on' else 0,
            frame,((frame+1)*MASTER+RATE-1)//RATE] for i,(route,frame) in enumerate(zip(ROUTES,positions))]
    notes=[]
    for begin,end in ((positions[0]+3200,positions[3]+128),(positions[4]+3200,positions[5]+128)):
        half=begin*2048000//RATE; off=end*2048000//RATE
        notes.append([2,2,1800,half,off,off+500,begin])
    meta=dict(schema='gbb-sgb-audio-transition-v1',qualification=False,playback=False,
              reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=4,version=0xDA,
              frames=frames,clocks=55000008,restore_count=320,gb_samples=115062,native_samples=81947,
              edges=edges,notes=notes)
    pcm=[]
    for i in range(frames):
        active=any(n[6]<=i<n[4]*RATE//2048000 for n in notes)
        snes=round(2000*math.sin(2*math.pi*440*i/RATE)) if active else 0
        stage=sum(i>=p for p in positions)
        route=edges[stage-1][1] if stage else 0
        gb=(1000 if math.sin(2*math.pi*(MASTER/5/8192)*i/RATE)>=0 else -1000) if route else 0
        pcm.append((snes+gb,snes))
    return meta,b''.join(struct.pack('<hh',*sample) for sample in pcm)


def controls():
    result={}
    for profile in PROFILES:
        meta,raw=synthetic(profile)
        result[profile]=observe(meta,raw,profile,2,2)
        result[profile]['meta']=meta
    return result


class AudioTransitionTests(unittest.TestCase):
    def test_owned_inputs_routing_and_reproducible_export(self):
        images=[build(p) for p in PROFILES]
        self.assertEqual(len(set(images)),3)
        for profile,image in zip(PROFILES,images):
            self.assertEqual(len(image),32768)
            self.assertEqual(image,build(profile))
            self.assertEqual(image[0x4000:],images[0][0x4000:])
            self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for voice,instrument in ((2,2),(3,10)):
            score,asset=objects(voice,instrument)
            self.assertEqual(len(score),2048);self.assertEqual(len(asset),192)
            for channel in (2,3):
                start=struct.unpack_from('<H',score,0x60+2*channel)[0]-0x2B00
                self.assertIn(bytes((64,127,0xA1 if channel==voice else 0xC9,0)),score[start:start+32])
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'owned.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_audio_transition_fixture.py'),
                     '--profile','toggle','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr)
                self.assertEqual(path.read_bytes(),images[0])

    def test_builder_rejects_unbounded_io_and_empty_actions(self):
        payloads=[bytes(4096)]; commands=[(1,None,None)]
        for writes in (None,[],[[]],[[(0,0)]],[[(0x26,True)]],[[(0x26,256)]],[[(0x80,1)]*13]):
            with self.subTest(writes=writes),self.assertRaises(ValueError):
                build_cartridge(payloads,commands,io_writes=writes)
        image=build_cartridge(payloads,commands,io_writes=[[(0x25,0),(0x80,1)]])
        self.assertEqual(len(image),32768)
        with self.assertRaises(ValueError):
            build_cartridge(payloads,[(1,None,0)],io_writes=[[(0x80,1)]])

    def test_matching_controls_and_muted_snes_continuation(self):
        result=controls()
        checked=compare(result,'sgb')
        self.assertTrue(checked['uninterrupted_snes_equal'])
        self.assertEqual(checked['maximum_phase_residual_step_lsb'],0)
        self.assertEqual(len(checked['gb_pulses']),4)

    def test_pcm_mutations_catch_stale_output_and_failed_restart(self):
        meta,raw=synthetic()
        for mutation in ('restart_silent','stuck_snes','stuck_gb','octave','clipped'):
            changed=bytearray(raw)
            if mutation=='restart_silent':
                begin=meta['notes'][1][6]+128
                for i in range(begin,begin+2048): struct.pack_into('<h',changed,4*i+2,0)
            elif mutation=='stuck_snes':
                begin=meta['edges'][3][2]+1024
                struct.pack_into('<h',changed,4*begin+2,500)
            elif mutation=='stuck_gb':
                begin=meta['edges'][1][2]+2816
                for i in range(begin,begin+128):
                    right=struct.unpack_from('<h',changed,4*i+2)[0]
                    struct.pack_into('<h',changed,4*i,right+500)
            elif mutation=='octave':
                begin=meta['notes'][0][6]+128
                for j in range(2048): struct.pack_into('<h',changed,4*(begin+j)+2,round(2000*math.sin(2*math.pi*880*j/RATE)))
            else:
                struct.pack_into('<h',changed,4*meta['notes'][0][6]+2,32767)
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                observe(meta,bytes(changed),'toggle',2,2)

    def test_phase_loss_rejects_even_with_correct_gb_frequency(self):
        result=controls()
        begin=result['toggle']['meta']['edges'][2][2]+512
        wave=result['on']['delta']
        result['toggle']['delta'][begin:begin+2048]=wave[begin+24:begin+24+2048]
        with self.assertRaises(ValueError):
            compare(result,'sgb')
        result=controls()
        result['silent']['right'][60000]=1
        with self.assertRaises(ValueError):
            compare(result,'sgb')

    def test_malformed_clock_edges_replay_and_note_metadata_reject(self):
        original,raw=synthetic()
        variants=[dict(original,restore_equal=False),dict(original,restore_count=True),dict(original,frames=1)]
        for mutation in ('missing','route','clock','note','gate','bool'):
            meta=copy.deepcopy(original)
            if mutation=='missing':meta['edges'].pop()
            elif mutation=='route':meta['edges'][1][1]=16
            elif mutation=='clock':meta['edges'][1][3]=meta['edges'][0][3]
            elif mutation=='note':meta['notes'][1][2]=1801
            elif mutation=='gate':meta['notes'][1][4]=meta['notes'][1][3]+1
            else:meta['edges'][1][2]=True
            variants.append(meta)
        for i,meta in enumerate(variants):
            with self.subTest(case=i),self.assertRaises(ValueError):observe(meta,raw,'toggle',2,2)
        with self.assertRaises(ValueError):observe(original,raw[:-4],'toggle',2,2)


if __name__=='__main__':
    unittest.main()
