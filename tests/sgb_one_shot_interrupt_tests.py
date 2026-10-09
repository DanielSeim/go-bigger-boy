#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public rejection guards for audible interruption and fresh transient restart."""
import copy
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_one_shot_interrupt_fixture import build,objects,PROFILES
from build_sgb_score_atomic_fixture import build_cartridge
from check_sgb_one_shot_interrupt import observe,compare,RATE,MASTER
from check_sgb_instrument_chromatic_reference import PITCHES


def synthetic(profile='both',held_voice=2):
    frames=122920;notes=[];completions=[];mask=0
    positions=(60000,64100,69000,100000)
    edges=[[i+1,0 if i==3 else 16,f,((f+1)*MASTER+RATE-1)//RATE] for i,f in enumerate(positions)]
    sources={2:[0]*frames,3:[0]*frames}
    for sequence,begin in enumerate((64000,72000)):
        for voice in (2,3):
            off=64140 if sequence==0 else 88000
            zero=off+2 if sequence==0 else off+(96 if voice==held_voice else 1)
            half=lambda frame:frame*2048000//RATE
            notes.append([voice,voice,PITCHES[2][9 if voice==held_voice else 0],half(begin),half(off),half(zero),
                          begin,off,zero,80 if sequence==0 else 60 if voice==held_voice else 0,
                          1<<voice if sequence and voice!=held_voice else 0])
            if sequence and voice!=held_voice:
                mask|=1<<(len(notes)-1)
                completions.append([half(begin+320),begin+320,half(begin+320),begin+320])
            else:completions.append([0]*4)
            if voice==held_voice:
                for i in range(begin,off):sources[voice][i]=round(2000*math.sin(2*math.pi*440*i/RATE))
            else:
                for i in range(begin+16,off+2 if sequence==0 else begin+284):sources[voice][i]=500 if i%16<8 else -500
    meta=dict(schema='gbb-sgb-one-shot-interrupt-v1',qualification=False,playback=False,reset_equal=True,restore_equal=True,
              sample_rate_hz=RATE,clipped=0,sounds=4,version=0xDA,restore_edges=15,restore_releases=15,restore_zeros=15,
              queued_onsets=15,queued_offs=15,restore_count=440,pending_restores=160,frames=frames,clocks=55000028,
              gb_samples=115062,native_samples=81947,edges=edges,notes=notes,completions=completions,
              restore_ends=mask,restore_natural_zeros=mask)
    pcm=[]
    for i in range(frames):
        gb=(1000 if math.sin(2*math.pi*(MASTER/5/8192)*i/RATE)>=0 else -1000) if positions[0]<=i<positions[-1] else 0
        snes=sum(sources[v][i] for v in (2,3) if profile in ('both',f'voice{v}'))
        pcm.append((gb,snes))
    return meta,b''.join(struct.pack('<hh',*x) for x in pcm)


def controls(held=2):return {p:observe(*synthetic(p,held),p,held) for p in PROFILES}


class OneShotInterruptTests(unittest.TestCase):
    def test_subframe_delay_bounds_default_and_opcode(self):
        payloads=[bytes(4096)];commands=[(1,bytes((0x41,0,0,0,1)),None)]
        original=build_cartridge(payloads,commands)
        self.assertEqual(original,build_cartridge(payloads,commands,spin_delays=[0]))
        for count in (1,1800,2499):
            image=build_cartridge(payloads,commands,spin_delays=[count])
            self.assertIn(bytes((1,count&255,count>>8,0x0B,0x78,0xB1,0x20,0xFB)),image[0x150:0x4000])
            self.assertEqual(image[0x4000:],original[0x4000:])
        for delays in ([],[True],[-1],[2500],[1,2],'1',[1.0]):
            with self.assertRaises(ValueError):build_cartridge(payloads,commands,spin_delays=delays)

    def test_fixture_geometry_code_checksums_and_export(self):
        for held in (2,3):
            score,original=objects('both',held)
            for profile in PROFILES:
                other,asset=objects(profile,held);self.assertEqual(other,score)
                expected=bytearray(original)
                for voice in (2,3):
                    slot=voice-2;start=struct.unpack_from('<H',original,8+4*slot)[0]-0x5000
                    if profile not in ('both',f'voice{voice}'):
                        for block in range(original[16+slot]):expected[start+9*block+1:start+9*block+9]=bytes(8)
                self.assertEqual(asset,bytes(expected))
                image=build(profile,held);self.assertEqual(image,build(profile,held));self.assertEqual(len(image),32768)
                self.assertEqual(image[0x150:0x4000],build('both',held)[0x150:0x4000])
                self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'interrupt.gb';command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_one_shot_interrupt_fixture.py'),
                                                  '--held-voice','3','--output',str(path)]
            for status in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,status,child.stderr);self.assertEqual(path.read_bytes(),build('both',3))

    def test_both_roles_stop_gap_and_fresh_playback(self):
        for held in (2,3):
            result=compare(controls(held),'sgb',held)
            self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
            self.assertEqual(len(result['transients']),2)

    def test_late_stop_false_endx_and_missing_queued_restore_reject(self):
        original,raw=synthetic()
        for mutation in ('late','inactive','endx','natural','missing','queued_on','queued_off','coverage','boolean','clock'):
            meta=copy.deepcopy(original)
            if mutation=='late':meta['edges'][1][2]=65000
            elif mutation=='inactive':meta['notes'][1][9]=0
            elif mutation=='endx':meta['notes'][1][10]=8
            elif mutation=='natural':meta['completions'][1]=meta['completions'][3][:]
            elif mutation=='missing':meta['notes'].pop()
            elif mutation=='queued_on':meta['queued_onsets']=7
            elif mutation=='queued_off':meta['queued_offs']=7
            elif mutation=='coverage':meta['restore_ends']=0
            elif mutation=='boolean':meta['notes'][1][9]=True
            else:meta['clocks']=1
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):observe(meta,raw,'both',2)
        with self.assertRaises(ValueError):observe(original,raw[:-4],'both',2)

    def test_false_audibility_stale_tail_and_missing_restart_reject(self):
        for mutation in ('inaudible_stop','restart','stale','held','gb','sum','timeline','unshortened'):
            result=controls();first=result['both']['peer'][0][0];second=result['both']['peer'][1][0]
            def change(voice,index,value):
                old=result[f'voice{voice}']['right'][index]
                result[f'voice{voice}']['right'][index]=value;result['both']['right'][index]+=value-old
            if mutation=='inaudible_stop':
                for i in range(first[7]-32,first[7]):change(3,i,0)
            elif mutation=='restart':
                for i in range(second[6],second[6]+400):change(3,i,0)
            elif mutation=='stale':change(3,65000,500)
            elif mutation=='held':
                for i in range(second[6],second[6]+2200):change(2,i,0)
            elif mutation=='gb':result['voice3']['left'][65000]+=500
            elif mutation=='sum':result['both']['right'][64000]+=5
            elif mutation=='timeline':result['voice3']['meta']['completions'][3][1]+=1
            else:
                # A spurious post-stop tail makes the first transient as long
                # as the fresh sample while preserving source superposition.
                for i in range(first[7],first[8]+32):change(3,i,500)
                # Shorten the fresh control to the same audible duration.
                for i in range(second[6]+170,second[6]+400):change(3,i,0)
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(result,'sgb',2)
        result=controls();result['both']['right'][64000]+=4
        self.assertEqual(compare(result,'sgb',2)['maximum_sum_error_lsb']['right'],4)


if __name__=='__main__':unittest.main()
