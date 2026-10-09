#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public guards for owned combined output and real PCM frequency evidence."""
import copy
import math
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import sgb_host_sample_pitch_tests as native
from build_sgb_combined_sample_pitch_fixture import build
from check_sgb_combined_sample_pitch import observe,FRAMES,RATE
from check_sgb_owned_sample_pitch import audible_pitch


def synthetic(model='sgb',active=False,voice=2,instrument=2,reverse=False):
    result=native.synthetic(voice,instrument,reverse)
    result['envelope_setup_changes']=[0]*13
    acoustic=result['acoustic']
    acoustic.update(sample_rate_hz=RATE,frames_per_window=FRAMES,gb_samples_captured=100000,clipped_samples=0)
    target=(21477273/5 if model=='sgb' else 4194304)/8192
    for index,(window,note) in enumerate(zip(acoustic['windows'],result['envelope_notes'])):
        hz=440*2**((index-9)/12)
        window['pcm']=[round(2000*math.sin(2*math.pi*hz*i/RATE)) for i in range(FRAMES)]
        pulse=[1000 if math.sin(2*math.pi*target*i/RATE)>=0 else -1000 for i in range(FRAMES)]
        window['left']=[a+(b if active else 0) for a,b in zip(window['pcm'],pulse)]
        window['start_half']=note[16]+32
        window['end_half']=note[16]+131200
    return result


class CombinedPitchTests(unittest.TestCase):
    def test_owned_entry_routing_payload_and_checksums(self):
        for active in (False,True):
            image=build(3,10,True,active)
            self.assertEqual(image,build(3,10,True,active))
            self.assertEqual(len(image),32768)
            original=native.build(3,10,True)
            self.assertEqual(image[0x4000:],original[0x4000:])
            self.assertEqual(image[0x150:0x3F00],original[0x150:0x3F00])
            self.assertEqual(image[0x100:0x103],bytes((0xC3,0,0x3F)))
            self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
            if active:
                self.assertIn(bytes((0x3E,0x10,0xE0,0x25)),image[0x3F00:0x3F40])
        with self.assertRaises(ValueError):
            build(active_gb=1)

    def test_export_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'combined.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_combined_sample_pitch_fixture.py'),
                     '--voice','3','--instrument','10','--reversed-map','--active-gb','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr)
                self.assertEqual(path.read_bytes(),build(3,10,True,True))

    def test_silent_and_active_pcm_on_both_model_clocks(self):
        for model in ('sgb','sgb2'):
            for active in (False,True):
                for voice,instrument,reverse in ((2,2,False),(3,10,True)):
                    rows=observe(synthetic(model,active,voice,instrument,reverse),voice,instrument,reverse,model,active)
                    self.assertEqual(len(rows),13)
                    self.assertLess(max(abs(row['error_cents']) for row in rows),.1)
                    self.assertEqual('gb_pulse' in rows[0],active)

    def test_pcm_mutations_reject_with_unchanged_pitch_registers(self):
        original=synthetic('sgb',True)
        variants=[]
        for key,value in (('clipped_samples',1),('gb_samples_captured',0),('sample_rate_hz',32000)):
            changed=copy.deepcopy(original)
            changed['acoustic'][key]=value
            variants.append(changed)
        changed=copy.deepcopy(original)
        changed['envelope_setup_changes'][0]=1
        variants.append(changed)
        for mutation in ('missing_gb','right_leak','wrong_snes_pitch','wrong_gb_pitch','clipped','gate','start','length','bool'):
            changed=copy.deepcopy(original)
            window=changed['acoustic']['windows'][0]
            if mutation=='missing_gb':
                window['left']=window['pcm'][:]
            elif mutation=='right_leak':
                window['pcm']=window['left'][:]
            elif mutation=='wrong_snes_pitch':
                hz=2*440*2**(-9/12)
                window['pcm']=[round(2000*math.sin(2*math.pi*hz*i/RATE)) for i in range(FRAMES)]
            elif mutation=='wrong_gb_pitch':
                window['left']=[x+round(1000*math.sin(2*math.pi*700*i/RATE)) for i,x in enumerate(window['pcm'])]
            elif mutation=='clipped':
                window['left'][100]=32767
            elif mutation=='gate':
                window['end_half']=changed['envelope_notes'][0][17]
            elif mutation=='start':
                window['start_half']=changed['envelope_notes'][0][16]-1
            elif mutation=='length':
                window['left'].pop()
            else:
                window['left'][100]=True
            variants.append(changed)
        for i,changed in enumerate(variants):
            with self.subTest(case=i),self.assertRaises(ValueError):
                observe(changed,2,2,False,'sgb',True)
        with self.assertRaises(ValueError):
            observe(original,2,2,False,'sgb',False)

    def test_rate_argument_rejects_unsupported_and_boolean_values(self):
        for rate in (True,48000.0,44100,0):
            with self.assertRaises(ValueError):
                audible_pitch([0]*FRAMES,frames=FRAMES,warmup=192,sample_rate=rate)


if __name__=='__main__':
    unittest.main()
