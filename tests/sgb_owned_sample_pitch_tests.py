#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PCM pitch, wrong-octave rejection, and real transport for owned sample tuning."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import check_sgb_owned_sample_pitch as pitch
from build_sgb_owned_sample_fixture import build,calibrated_assets
import sgb_score_peer_gate_tests as peer
from sgb_score_atomic_tests import fnv
from build_sgb_score_transport import build as program
RUNNER=PROBE=None
ASSET_HASH='97a71e49a8b62073aae00e0ac1f5ef6830affcce3c28f1be8b59be6cf505c12b'


class OwnedSamplePitchTests(unittest.TestCase):
    def test_asset_geometry_and_independent_single_cycle_waves(self):
        asset=calibrated_assets()
        self.assertEqual(len(asset),192)
        self.assertEqual(hashlib.sha256(asset).hexdigest(),ASSET_HASH)
        self.assertEqual(asset[:8],bytes((2,0x8F,0x6F,0xB8,3,0x8E,0xAF,0xB8)))
        self.assertEqual(asset[18:26],bytes((0,0,0,0,0,0,2,10)))
        for index in (0,1):
            start,loop=struct.unpack_from('<HH',asset,8+4*index)
            self.assertEqual(start,loop)
            self.assertEqual(asset[16+index],2)
            wave=[]
            for block in range(2):
                base=start-0x5000+9*block
                self.assertEqual(asset[base],0xB3 if block else 0xB0)
                for value in asset[base+1:base+9]:
                    wave += [n-16 if n>=8 else n for n in (value>>4,value&15)]
            self.assertEqual(len(wave),32)
            self.assertEqual(sum(wave),0)
            self.assertEqual(sum(a<0<=b for a,b in zip(wave,wave[1:]+wave[:1])),1)
            self.assertFalse(any(all(wave[i]==wave[(i+period)%32] for i in range(32)) for period in (1,2,4,8,16)))

    def test_estimator_frequency_and_capture_rejections(self):
        samples=[round(2000*math.sin(2*math.pi*i/64)) for i in range(pitch.FRAMES)]
        result=pitch.audible_pitch(samples)
        self.assertAlmostEqual(result['frequency_hz'],500,places=7)
        for values in ([0]*pitch.FRAMES,[200]*pitch.FRAMES,[True]*pitch.FRAMES,samples[:-1],
                       [float('nan')]*pitch.FRAMES,
                       [32767 if i%64<32 else -32768 for i in range(pitch.FRAMES)],
                       [2000 if i%16<8 else -2000 for i in range(pitch.FRAMES)],
                       [round(2000*math.sin(2*math.pi*(i/64+i*i/1000000))) for i in range(pitch.FRAMES)]):
            with self.assertRaises(ValueError):
                pitch.audible_pitch(values)

    def test_stimulus_bounds_and_exporter(self):
        asset=calibrated_assets()
        for args in ((asset,True,1068),(asset,2,0),(asset,2,16384),(asset,2,True),
                     (asset[:-1],2,1068),(asset,2,1068,'unknown')):
            with self.assertRaises(ValueError):
                pitch.stimulus(*args)
        for note in (23,37,True,24.0):
            with self.assertRaises(ValueError):
                build(note)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'owned.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_owned_sample_fixture.py'),'--note','36','--output',str(path)]
            for code in (0,2):
                child=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(child.returncode,code,child.stderr)
                self.assertEqual(path.read_bytes(),build(36))

    def test_pcm_calibration_on_both_slots_envelopes_and_full_octave(self):
        if RUNNER is None:
            self.skipTest('DSP fixture runner required')
        report=pitch.measure(RUNNER)
        self.assertEqual(len(report['observations']),52)
        self.assertEqual(report['asset_sha256'],ASSET_HASH)
        self.assertLessEqual(report['maximum_absolute_error_cents'],10)
        self.assertIs(report['qualification'],False)
        self.assertIs(report['playback'],False)
        self.assertEqual({(row['slot'],row['envelope'],row['base_note']) for row in report['observations']},
                         {(slot,envelope,note) for slot in (2,3) for envelope in ('gain','adsr') for note in range(24,37)})
        self.assertEqual(len({row['pcm_sha256'] for row in report['observations']}),52)
        # Repeat one physical render: detector success alone is not replay evidence.
        fixture=pitch.stimulus(calibrated_assets(),3,1068,'adsr')
        self.assertEqual(pitch.render(RUNNER,fixture),pitch.render(RUNNER,fixture))

    def test_pcm_wrong_tuning_and_shorter_wave_reject(self):
        if RUNNER is None:
            self.skipTest('DSP fixture runner required')
        with patch.dict(pitch.PITCHES,{2:pitch.PITCHES[10]}),self.assertRaises(ValueError):
            pitch.measure(RUNNER)
        changed=bytearray(calibrated_assets())
        # Independently specify a 16-sample square in each block: the register
        # pitch stays unchanged, but the audible fundamental moves an octave.
        for index in (0,1):
            start=struct.unpack_from('<H',changed,8+4*index)[0]-0x5000
            for block in (0,1):
                changed[start+9*block+1:start+9*block+9]=bytes((0x99,)*4+(0x77,)*4)
        with patch.object(pitch,'calibrated_assets',return_value=bytes(changed)),self.assertRaises(ValueError):
            pitch.measure(RUNNER)

    def test_stress_assets_reject_as_known_single_cycle_calibration(self):
        if RUNNER is None:
            self.skipTest('DSP fixture runner required')
        from build_sgb_score_adsr_fixture import objects
        _,stress=objects()
        for slot,word in ((2,pitch.PITCHES[2][0]),(3,pitch.PITCHES[10][0])):
            samples,_=pitch.render(RUNNER,pitch.stimulus(stress,slot,word))
            with self.subTest(slot=slot),self.assertRaises(ValueError):
                pitch.audible_pitch(samples)

    def test_real_upload_reset_save_load_and_scalar_parity(self):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            host,game=Path(directory)/'host.rom',Path(directory)/'game.gb'
            host.write_bytes(program(**peer.PeerGateTests.BUILD_OPTIONS))
            game.write_bytes(build())
            for model in ('sgb','sgb2'):
                runs=[]
                for mode in ('native','scalar'):
                    child=subprocess.run([str(PROBE),str(host),str(game),model,'70000000',mode],
                                         capture_output=True,text=True,timeout=90)
                    self.assertEqual(child.returncode,0,child.stderr)
                    self.assertLess(len(child.stdout),16384)
                    result=json.loads(child.stdout)
                    for key in ('reset_equal','restore_equal'):
                        self.assertIs(result[key],True)
                    self.assertEqual((result['status'],result['version'],result['error'],result['external']),
                                     (1,0xDA,0,0))
                    self.assertEqual(result['asset_hash'],fnv(calibrated_assets()))
                    self.assertEqual(result['sample_counts'],[2,2])
                    self.assertEqual(result['profile_pitch'],[1068,1068])
                    self.assertGreater(result['pcm']['nonzero_frames'],100)
                    self.assertEqual(len(result['envelope_notes']),2)
                    self.assertTrue(all(row[5:7]==[0,1068] for row in result['envelope_notes']))
                    self.assertTrue(1<=result['restore_count']<=4096)
                    runs.append(result)
                self.assertEqual(runs[0]['pcm'],runs[1]['pcm'])
                self.assertEqual(runs[0]['envelope_notes'],runs[1]['envelope_notes'])


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--runner',type=Path)
    parser.add_argument('--probe',type=Path)
    args,remaining=parser.parse_known_args()
    RUNNER=args.runner.resolve() if args.runner else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0],*remaining])
