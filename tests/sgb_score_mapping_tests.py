#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical score-ID mapping, normalized caches, inheritance and recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_score_atomic_tests as atomic
import sgb_score_recovery_tests as recovery
import sgb_score_instrument_profiles_tests as profiles_tests

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_score_transport import build as program
from build_sgb_score_mapping_fixture import build as cartridge, objects, payload, FAULTS
from build_sgb_score_relocated_fixture import build as legacy_cartridge
PROBE = None
OPTIONS = {**recovery.OPTIONS,'upload_recovery':True}
IMAGE_HASH = '6b2fdf4494e7715c6899335ebe34f39dcd62728cb6fd3a1d34f562d629ee9607'


class MappingTests(unittest.TestCase):
    def probe(self, model, order=(1,), clocks=45000000, mode='native', mapping=True,
              legacy=False, **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom,game = Path(directory)/'host.rom',Path(directory)/'game.gb'
            rom.write_bytes(program(**OPTIONS,instrument_mapping=mapping))
            game.write_bytes(legacy_cartridge(order,sample_profile='mixed3',profile='distinct',
                                             score_profile='switch',layout='near') if legacy else
                             cartridge(order,**fixture))
            child = subprocess.run([str(PROBE),str(rom),str(game),model,str(clocks),mode],
                                   capture_output=True,text=True,timeout=90)
            self.assertEqual(child.returncode,0,child.stderr)
            self.assertLess(len(child.stdout),4096)
            result = json.loads(child.stdout)
            self.assertEqual(result['schema'],'gbb-score-transport-v1')
            for field in ('qualification','playback'):
                self.assertIs(result[field],False)
            for field in ('reset_equal','restore_equal'):
                self.assertIs(result[field],True)
            self.assertLessEqual(result['restore_count'],4096)
            self.assertLessEqual(result['pcm']['frames'],250000)
            self.assertLessEqual(result['clocks']-clocks,256)
            return result

    def regions(self, result, ids=(2,10), **options):
        score,data = objects(ids,**options)
        self.assertEqual(result['score_hash'],atomic.fnv(score))
        self.assertEqual(result['asset_hash'],atomic.fnv(data))
        self.assertEqual(result['sample_starts'],[int.from_bytes(data[e:e+2],'little') for e in (8,12)])
        self.assertEqual(result['sample_loops'],[int.from_bytes(data[e:e+2],'little') for e in (10,14)])
        self.assertEqual(result['sample_modes'],[0,1])

    def ready(self, result, song=1, transfers=1, adoptions=None, sounds=2,
              attempts=0, suppressed=0, events=None):
        self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                          result['version'],result['bridge'],result['signature'],
                          result['error'],result['external'],result['selected_song'],
                          result['admitted_roots'],result['sounds']),
                         (1,transfers,transfers+1 if adoptions is None else adoptions,
                          0xD6,2,0xA5,0,0,song,3,sounds))
        self.assertEqual(result['score_tick'],76 if song == 3 else 72)
        self.assertEqual(result['restore_roots'],7)
        self.assertEqual(result['atomic_phase'],0xA5)
        self.assertEqual(result['voice_env'],[0,0])
        self.assertGreater(result['pcm']['nonzero_frames'],100)
        self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)
        recovery.RecoveryTests.evidence(self,result,attempts,transfers if events is None else events,suppressed)

    def test_caps_previous_images_and_reproducible_exports(self):
        image = program(**OPTIONS,instrument_mapping=True)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),IMAGE_HASH)
        self.assertEqual(hashlib.sha256(program(**OPTIONS)).hexdigest(),
                         'bcbe4765596df4db0232bb23526c804bc9f7812a796cd16b2457cf697769a357')
        for flags in ({'instrument_mapping':True},{**OPTIONS,'instrument_mapping':1}):
            with self.assertRaises(ValueError):
                program(**flags)
        for flags in ({'ids':(2,2)},{'ids':(True,10)},{'ids':(2,256)},
                      {'ids':(2,)},{'ids':(2,-1)},{'fault':'unknown'},
                      {'active':1},{'recover':True},{'repeat_failure':True},
                      {'fault':'duplicate','order':(2,)},{'replace':True},
                      {'replace':True,'order':(1,3),'repeat_upload':True},
                      {'replacement_score_profile':'switch'}):
            with self.assertRaises(ValueError):
                cartridge(**flags)
        for ids in ((0,255),(224,229),(2,10),(10,2)):
            score,data = objects(ids)
            self.assertEqual((len(score),len(data)),(2048,192))
            self.assertEqual(data[24:26],bytes(ids))
            self.assertEqual((data[0],data[4]),(2,3))
            self.assertEqual((score[0x2F2],score[0x4F6]),ids)
        with tempfile.TemporaryDirectory() as directory:
            cases = (
                ('build_sgb_score_transport.py',['--'+k.replace('_','-') for k in (*OPTIONS,'instrument_mapping')],image),
                ('build_sgb_score_mapping_fixture.py',['--ids','0,255','--score-profile','maximum'],
                 cartridge(ids=(0,255),score_profile='maximum')),
                ('build_sgb_score_mapping_fixture.py',['--fault','unknown-third','--recover','--repeat-failure',
                                                       '--active-failure','--stop','--chunking','tail'],
                 cartridge(fault='unknown-third',recover=True,repeat_failure=True,
                           active_failure=True,stop=True,chunking='tail')),
                ('build_sgb_score_mapping_fixture.py',['--order','1,3','--replace','--score-profile','all2',
                                                       '--replacement-score-profile','switch'],
                 cartridge((1,3),replace=True,score_profile='all2',replacement_score_profile='switch')))
            for index,(script,flags,expected) in enumerate(cases):
                path = Path(directory)/f'{index}.bin'
                cmd = [sys.executable,str(ROOT/'scripts'/script),*flags,'--output',str(path)]
                child = subprocess.run(cmd,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,0,child.stderr)
                self.assertEqual(path.read_bytes(),expected)
                self.assertNotEqual(subprocess.run(cmd,capture_output=True,timeout=10).returncode,0)
                self.assertEqual(path.read_bytes(),expected)

    def test_selector_renaming_keeps_pcm_and_resolved_slots(self):
        for model in ('sgb','sgb2'):
            baseline = None
            hashes = set()
            for ids in ((2,10),(10,2),(0,255),(224,229),(2,3)):
                with self.subTest(model=model,ids=ids):
                    result = self.probe(model,ids=ids)
                    self.ready(result)
                    self.regions(result,ids)
                    self.assertEqual(result['instruments'],15)
                    self.assertEqual(result['prefix_ids'],[2,0,0,3,3,0,0,2,0])
                    self.assertEqual(result['prefix_counts'],[1,1,1,1,0,0,0,0,0])
                    profiles_tests.InstrumentProfilesTests.voice_profiles(self,result,'distinct',(3,2))
                    hashes.add(result['score_hash'])
                    if baseline is None:
                        baseline = result
                    else:
                        for field in ('pcm','prefix_ids','prefix_counts','voice_profiles','voice_pitch',
                                      'voice_tuning','end_sources','end_env','instruments'):
                            self.assertEqual(result[field],baseline[field])
            self.assertEqual(len(hashes),5)

    def test_render_modes_and_voice_local_inheritance(self):
        profiles = (('inherit',14,(3,3)),('default',7,(3,2)),('repeat',15,(3,2)),
                    ('maximum',14,(3,2)),('end',15,(3,3)),('clipped',15,(3,2)),
                    ('all2',5,(2,2)),('all3',10,(3,3)))
        for model in ('sgb','sgb2'):
            for score,mask,sources in profiles:
                with self.subTest(model=model,score=score):
                    result = self.probe(model,score_profile=score)
                    self.ready(result)
                    self.regions(result,score_profile=score)
                    self.assertEqual(result['instruments'],mask)
                    profiles_tests.InstrumentProfilesTests.voice_profiles(self,result,'distinct',sources)
                    if score == 'maximum':
                        # Voice 2's 26-prefix chain ends in slot 3 and never plays slot 2.
                        self.assertEqual(result['prefix_counts'][:4],[26,1,27,1])
            baseline = None
            for mode in ('native','scalar','combined'):
                result = self.probe(model,mode=mode)
                self.ready(result)
                if baseline is None:
                    baseline = result
                elif mode == 'scalar':
                    self.assertEqual(result['pcm'],baseline['pcm'])
                else:
                    self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2),2)

    def test_reordered_selection_stop_and_reupload(self):
        for model in ('sgb','sgb2'):
            for order,flags,transfers,sounds in (
                    ((3,1,2,3),{},1,8),((1,128,2,3),{},1,8),
                    ((3,1,2),{'active':True,'repeat_upload':True},3,2)):
                result = self.probe(model,order,clocks=125000000,**flags)
                self.ready(result,order[-1],transfers=transfers,sounds=sounds)
                self.regions(result)

    def test_changed_mapping_generation(self):
        for model in ('sgb','sgb2'):
            for ids in ((10,2),(0,255),(224,229)):
                for stop in (False,True):
                    result = self.probe(model,(1,3),replace=True,replacement_ids=ids,
                                        stop=stop,clocks=100000000,score_profile='all2',
                                        replacement_score_profile='switch')
                    self.ready(result,3,transfers=2)
                    self.regions(result,ids)
                    self.assertEqual(result['interruptions'],1 if stop else 4)
                    self.assertEqual(result['active_env'],result['interruptions'])
                    self.assertGreater(result['interrupt_tick'],0)
                    self.assertLess(result['interrupt_tick'],32)

    def test_rejects_ambiguous_unknown_and_unowned_fields(self):
        for model in ('sgb','sgb2'):
            for fault in FAULTS:
                with self.subTest(model=model,fault=fault):
                    result = self.probe(model,fault=fault)
                    preflight = fault == 'mapping-only'
                    self.assertEqual((result['status'],result['version'],result['bridge'],
                                      result['signature'],result['error'],result['transfers'],
                                      result['adoptions'],result['admitted_roots'],result['selected_song']),
                                     (9,0xD6,0xE2,0xA5,1 if preflight else 2,0 if preflight else 1,1,0,0))
                    recovery.RecoveryTests.evidence(self,result,1,1,1,blocked=1)
                    self.assertEqual(result['pcm']['nonzero_frames'],0)
                    self.assertEqual(result['atomic_phase'],0xA4)
                    self.assertEqual(result['restore_roots'],3 if fault == 'unknown-third' else 0)
                    if preflight:
                        self.assertEqual(result['score_hash'],atomic.fnv(bytes(2048)))
                        self.assertEqual(result['asset_hash'],atomic.fnv(bytes(192)))
                    else:
                        self.regions(result,fault=fault)

    def test_retry_replaces_mapping_and_partial_admission(self):
        for model in ('sgb','sgb2'):
            for fault in ('duplicate','unknown-third','mapping-only'):
                for warm in (False,True):
                    for mode in ('native','combined'):
                        result = self.probe(model,fault=fault,recover=True,active_failure=warm,
                                            repeat_failure=True,stop=True,mode=mode,clocks=100000000)
                        transfers = int(warm)+1+(0 if fault == 'mapping-only' else 2)
                        self.ready(result,3 if warm else 1,transfers=transfers,adoptions=2+int(warm),
                                   attempts=2,suppressed=3,events=3+int(warm))
                        self.regions(result,(10,2))

    def test_fragmented_semantic_retries_use_consumed_token(self):
        for model in ('sgb','sgb2'):
            for chunks in ('tail','reverse','split','interleave'):
                result = self.probe(model,fault='unknown-third',recover=True,
                                    repeat_failure=True,chunking=chunks,clocks=100000000)
                self.ready(result,transfers=3,adoptions=2,attempts=2,suppressed=2,events=3)
                self.regions(result,(10,2))

    def test_previous_profile_remains_frozen(self):
        for model in ('sgb','sgb2'):
            result = self.probe(model,mapping=False)
            self.assertEqual((result['status'],result['version'],result['bridge'],
                              result['recovery_rejections'],result['admitted_roots']),
                             (9,0xD5,0xE2,1,0))
            self.assertEqual(result['pcm']['nonzero_frames'],0)
            result = self.probe(model,mapping=False,legacy=True)
            self.assertEqual((result['status'],result['version'],result['bridge'],result['transfers'],
                              result['adoptions'],result['admitted_roots']),
                             (1,0xD5,2,1,2,3))
            self.assertGreater(result['pcm']['nonzero_frames'],100)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0],*remaining])
