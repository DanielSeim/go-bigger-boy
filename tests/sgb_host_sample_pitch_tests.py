#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned octave geometry and PCM-dependent native capture guards."""
import copy
import math
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_host_sample_pitch_fixture import build,objects
from check_sgb_host_sample_pitch import observe,fnv,FRAMES
from check_sgb_instrument_chromatic_reference import PITCHES,SETUPS


def synthetic(voice=2,instrument=2,reversed_map=False):
    score,asset=objects(voice,instrument,reversed_map)
    slot=((10,2) if reversed_map else (2,10)).index(instrument)+2
    windows,notes=[],[]
    for index,pitch in enumerate(PITCHES[2]):
        hz=440*2**((index-9)/12)
        pcm=[round(2000*math.sin(2*math.pi*hz*i/32000)) for i in range(FRAMES)]
        windows.append({'voice':voice,'slot':slot,'pitch':pitch,'on_sample':1000+index*3000,'pcm':pcm})
        note=[0]*30
        note[:7]=[voice,slot,*SETUPS[instrument][1:],0,pitch]
        note[16]=1000000+index*180000
        note[17]=note[16]+64*2300
        notes.append(note)
    return {'schema':'gbb-score-transport-v1','qualification':False,'playback':False,
            'reset_equal':True,'restore_equal':True,'status':1,'version':0xDA,'error':0,'external':0,
            'transfers':1,'adoptions':2,'bridge':2,'signature':0xA5,'selected_song':1,'admitted_roots':3,
            'score_tick':208,'score_hash':fnv(score),'asset_hash':fnv(asset),'restore_count':200,
            'acoustic':{'sample_rate_hz':32000,'frames_per_window':FRAMES,'windows':windows},
            'envelope_notes':notes}


class HostSamplePitchTests(unittest.TestCase):
    def test_owned_octave_muting_and_reversible_map(self):
        images=set()
        for voice in (2,3):
            for instrument in (2,10):
                for reverse in (False,True):
                    score,asset=objects(voice,instrument,reverse)
                    slot=((10,2) if reverse else (2,10)).index(instrument)+2
                    self.assertEqual(asset[24:26],bytes((10,2) if reverse else (2,10)))
                    self.assertEqual(asset[1+4*(slot-2):4+4*(slot-2)],bytes(SETUPS[instrument][1:]))
                    for pattern,notes in ((0,range(24,31)),(1,range(31,37))):
                        for channel in (2,3):
                            start=struct.unpack_from('<H',score,0x60+pattern*16+channel*2)[0]-0x2B00
                            body=score.index(bytes((16,127)),start,start+32)+2
                            self.assertEqual(score[body:body+len(notes)],bytes(0x80+note for note in notes)
                                             if channel==voice else bytes((0xC9,))*len(notes))
                    image=build(voice,instrument,reverse)
                    self.assertEqual(len(image),32768)
                    self.assertEqual(image,build(voice,instrument,reverse))
                    images.add(image)
        self.assertEqual(len(images),8)
        for args in ((True,2,False),(4,2,False),(2,3,False),(2,10,1)):
            with self.assertRaises(ValueError):
                build(*args)

    def test_export_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'octave.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_host_sample_pitch_fixture.py'),
                     '--voice','3','--instrument','10','--reversed-map','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr)
                self.assertEqual(path.read_bytes(),build(3,10,True))

    def test_synthetic_pcm_and_all_voice_instrument_map_combinations(self):
        for voice in (2,3):
            for instrument in (2,10):
                for reverse in (False,True):
                    rows=observe(synthetic(voice,instrument,reverse),voice,instrument,reverse)
                    self.assertEqual(len(rows),13)
                    self.assertLess(max(abs(row['error_cents']) for row in rows),.1)
                    self.assertFalse(any('pcm' in row for row in rows))

    def test_pcm_octave_error_rejects_despite_correct_register_pitch(self):
        report=synthetic()
        hz=2*440*2**(-9/12)
        report['acoustic']['windows'][0]['pcm']=[round(2000*math.sin(2*math.pi*hz*i/32000)) for i in range(FRAMES)]
        with self.assertRaises(ValueError):
            observe(report,2,2,False)

    def test_malformed_identity_windows_timing_and_pcm_reject(self):
        original=synthetic()
        variants=[dict(original,restore_equal=False),dict(original,restore_count=True),
                  dict(original,version=0xD9),dict(original,asset_hash=0)]
        for key,value in (('sample_rate_hz',48000),('sample_rate_hz',32000.0),('frames_per_window',8192),('windows',[])):
            changed=copy.deepcopy(original)
            changed['acoustic'][key]=value
            variants.append(changed)
        for key,value in (('voice',3),('voice',True),('slot',3),('pitch',7993),('on_sample',-1),
                          ('pcm',[0]*FRAMES),('pcm',[True]*FRAMES),('pcm',[float('nan')]*FRAMES)):
            changed=copy.deepcopy(original)
            changed['acoustic']['windows'][0][key]=value
            variants.append(changed)
        for change in ('missing','reordered','duplicate','early_gate','changed_setup','missed_sample'):
            changed=copy.deepcopy(original)
            if change=='missing':
                changed['envelope_notes'].pop()
            elif change=='reordered':
                changed['acoustic']['windows'].reverse()
            elif change=='duplicate':
                changed['acoustic']['windows'][1]=changed['acoustic']['windows'][0]
            elif change=='early_gate':
                changed['envelope_notes'][0][17]=changed['envelope_notes'][0][16]+64*FRAMES
            elif change=='changed_setup':
                changed['envelope_notes'][0][2]=0
            else:
                changed['envelope_notes'][0][15]=1
            variants.append(changed)
        for index,changed in enumerate(variants):
            with self.subTest(case=index),self.assertRaises(ValueError):
                observe(changed,2,2,False)


if __name__=='__main__':
    unittest.main()
