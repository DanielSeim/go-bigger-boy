#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact-table tuning, mapped slots, voice inheritance and physical lifecycle."""
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
import sgb_score_mapping_tests as mapping

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_score_transport import build as program
from build_sgb_score_tuning_fixture import build as cartridge, objects, FAULTS
from check_sgb_instrument_chromatic_reference import PITCHES
from build_sgb_score_instrument_profiles_fixture import PROFILES
PROBE = None
OPTIONS = {**mapping.OPTIONS,'instrument_mapping':True}
IMAGE_HASH = '61cac6e3c1dbcfe40318714a6044bd68bc65170302e6c0fd3c64b0570efd8fdb'


def pitch(note, selector):
    return PITCHES[10][note-24] if selector == 2 else PITCHES[2][note-24] >> selector


class TuningTests(unittest.TestCase):
    def probe(self, model, order=(1,), clocks=45000000, mode='native', tuning_image=True, **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom,game = Path(directory)/'host.rom',Path(directory)/'game.gb'
            rom.write_bytes(program(**OPTIONS,instrument_tuning=tuning_image))
            game.write_bytes(cartridge(order,**fixture))
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

    def ready(self, result, song=1, transfers=1, adoptions=None, sounds=2,
              attempts=0, suppressed=0, events=None, octave=False):
        self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                          result['version'],result['bridge'],result['signature'],
                          result['error'],result['external'],result['selected_song'],
                          result['admitted_roots'],result['sounds']),
                         (1,transfers,transfers+1 if adoptions is None else adoptions,
                          0xD7,2,0xA5,0,0,song,3,sounds))
        self.assertEqual(result['score_tick'],208 if octave else 76 if song == 3 else 72)
        self.assertEqual(result['restore_roots'],7)
        self.assertEqual(result['atomic_phase'],0xA5)
        self.assertEqual(result['voice_env'],[0,0])
        self.assertGreater(result['pcm']['nonzero_frames'],100)
        self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)
        recovery.RecoveryTests.evidence(self,result,attempts,transfers if events is None else events,suppressed)

    def voices(self, result, sources, tuning=(2,0), notes=(33,27)):
        self.assertEqual(result['voice_profiles'],[field for slot in sources for field in
                         (slot,*PROFILES['distinct'][slot-2][:3])])
        selectors = [tuning[slot-2] for slot in sources]
        self.assertEqual(result['voice_tuning'],selectors)
        self.assertEqual(result['voice_pitch'],[pitch(note,selector) for note,selector in zip(notes,selectors)])

    def test_caps_and_frozen_images_and_exports(self):
        image = program(**OPTIONS,instrument_tuning=True)
        self.assertEqual(hashlib.sha256(image).hexdigest(),IMAGE_HASH)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(program(**OPTIONS)).hexdigest(),mapping.IMAGE_HASH)
        for flags in ({'instrument_tuning':True},{**OPTIONS,'instrument_tuning':1}):
            with self.assertRaises(ValueError):
                program(**flags)
        for flags in ({'tuning':(2,3)},{'tuning':(True,0)},{'tuning':(2,)},
                      {'tuning':(2.0,0)},{'octave_slots':(1,3)},{'fault':'unknown'},
                      {'replacement_tuning':(0,2)}):
            with self.assertRaises(ValueError):
                cartridge(**flags)
        with tempfile.TemporaryDirectory() as directory:
            for i,(script,flags,expected) in enumerate((
                    ('build_sgb_score_transport.py',['--'+key.replace('_','-') for key in (*OPTIONS,'instrument_tuning')],image),
                    ('build_sgb_score_tuning_fixture.py',['--score-profile','octave','--tuning','1,2','--ids','0,255',
                     '--octave-slots','3,2'],cartridge(score_profile='octave',tuning=(1,2),ids=(0,255),octave_slots=(3,2))),
                    ('build_sgb_score_tuning_fixture.py',['--fault','tuning2-255','--recover','--repeat-failure',
                     '--chunking','tail','--replacement-tuning','0,2'],cartridge(fault='tuning2-255',recover=True,
                     repeat_failure=True,chunking='tail',replacement_tuning=(0,2))))):
                path = Path(directory)/f'{i}.bin'
                cmd = [sys.executable,str(ROOT/'scripts'/script),*flags,'--output',str(path)]
                child = subprocess.run(cmd,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,0,child.stderr)
                self.assertEqual(path.read_bytes(),expected)
                self.assertNotEqual(subprocess.run(cmd,capture_output=True,timeout=10).returncode,0)
                self.assertEqual(path.read_bytes(),expected)

    def test_full_octave_both_slots_voices_and_mixed_profiles(self):
        cases = (((2,0),(2,3),(2,10)),((0,2),(2,3),(10,2)),
                 ((2,1),(3,2),(0,255)),((1,2),(3,2),(224,229)))
        for model in ('sgb','sgb2'):
            for tuning,slots,ids in cases:
                with self.subTest(model=model,tuning=tuning,slots=slots):
                    result = self.probe(model,clocks=70000000,score_profile='octave',tuning=tuning,
                                        octave_slots=slots,ids=ids)
                    self.ready(result,octave=True)
                    self.regions(result,ids,score_profile='octave',tuning=tuning,octave_slots=slots)
                    self.assertEqual(result['tuning_seen'],8191)
                    self.assertEqual(result['tuning_pitches'],[pitch(note,tuning[slot-2])
                                     for note in range(24,37) for slot in slots])
                    self.assertEqual(result['voice_tuning'],[tuning[slot-2] for slot in slots])
                    self.assertEqual(result['voice_pitch'],[pitch(36,tuning[slot-2]) for slot in slots])

    def test_new_table_in_all_render_modes(self):
        for model in ('sgb','sgb2'):
            baseline = None
            for mode in ('native','scalar','combined'):
                result = self.probe(model,mode=mode,clocks=70000000,score_profile='octave',tuning=(2,2))
                self.ready(result,octave=True)
                self.assertEqual(result['tuning_seen'],8191)
                self.assertEqual(result['tuning_pitches'],[word for word in PITCHES[10] for _ in (2,3)])
                if baseline is None:
                    baseline = result
                elif mode == 'scalar':
                    self.assertEqual(result['pcm'],baseline['pcm'])
                else:
                    self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2),2)

    def test_voice_local_inheritance_ordered_prefixes_and_clipping(self):
        for model in ('sgb','sgb2'):
            for score,sources in (('switch',(3,2)),('inherit',(3,3)),('default',(3,2)),
                    ('repeat',(3,2)),('maximum',(3,2)),('end',(3,3)),('clipped',(3,2))):
                result = self.probe(model,score_profile=score)
                self.ready(result)
                self.regions(result,score_profile=score)
                self.voices(result,sources)
                first = (3,2) if score == 'inherit' else (2,2) if score == 'default' else (3,3) if score == 'maximum' else (2,3)
                self.assertEqual(result['profile_pitch'],[pitch(24,(2,0)[slot-2]) for slot in first])
                if score == 'maximum':
                    self.assertEqual(result['prefix_counts'][:4],[26,1,27,1])

    def test_active_replacement_stop_and_reupload(self):
        for model in ('sgb','sgb2'):
            for stop in (False,True):
                result = self.probe(model,(1,3),replace=True,stop=stop,clocks=100000000,
                                    score_profile='all2',replacement_score_profile='switch',
                                    replacement_ids=(10,2),replacement_tuning=(1,2))
                self.ready(result,3,transfers=2)
                self.regions(result,(10,2),score_profile='switch',tuning=(1,2))
                self.voices(result,(3,2),(1,2))
                self.assertEqual(result['interruptions'],1 if stop else 4)
                self.assertEqual(result['active_env'],result['interruptions'])
            result = self.probe(model,(3,1,2),repeat_upload=True,active=True,clocks=125000000)
            self.ready(result,2,transfers=3)
            self.regions(result)
            self.voices(result,(3,2),notes=(32,27))

    def test_rejects_bad_tuning_notes_and_reserved_fields(self):
        for model in ('sgb','sgb2'):
            for fault in ('tuning2-3','tuning3-3','tuning2-255','tuning3-255','reserved','note-low','note-high'):
                with self.subTest(model=model,fault=fault):
                    score = 'octave' if fault.startswith('note-') else 'switch'
                    result = self.probe(model,fault=fault,score_profile=score)
                    self.assertEqual((result['status'],result['version'],result['bridge'],
                                      result['signature'],result['error'],result['admitted_roots']),
                                     (9,0xD7,0xE2,0xA5,2,0))
                    self.assertEqual(result['pcm']['nonzero_frames'],0)
                    self.assertEqual(result['restore_roots'],0)
                    self.assertEqual(result['atomic_phase'],0xA4)
                    self.regions(result,fault=fault,score_profile=score)
                    recovery.RecoveryTests.evidence(self,result,1,1,1,blocked=1)

    def test_fragmented_cold_and_active_recovery(self):
        for model in ('sgb','sgb2'):
            for warm,fault,chunk in ((False,'tuning2-255','tail'),(True,'unknown-third','interleave')):
                result = self.probe(model,fault=fault,recover=True,active_failure=warm,
                                    repeat_failure=True,stop=True,chunking=chunk,clocks=100000000,
                                    score_profile='all2',replacement_score_profile='switch',replacement_tuning=(0,2))
                self.ready(result,3 if warm else 1,transfers=3+int(warm),adoptions=2+int(warm),
                           attempts=2,suppressed=3,events=3+int(warm))
                self.regions(result,(10,2),score_profile='switch',tuning=(0,2))
                self.voices(result,(3,2),(0,2))

    def test_frozen_d6_and_normal_half_control(self):
        for model in ('sgb','sgb2'):
            result = self.probe(model,tuning_image=False)
            self.assertEqual((result['status'],result['version'],result['bridge'],result['admitted_roots']),
                             (9,0xD6,0xE2,0))
            self.assertEqual(result['pcm']['nonzero_frames'],0)
            result = self.probe(model,tuning=(1,0))
            self.ready(result)
            self.voices(result,(3,2),(1,0))
            prior = self.probe(model,tuning_image=False,tuning=(1,0))
            self.assertEqual((prior['status'],prior['version'],prior['bridge'],prior['admitted_roots']),
                             (1,0xD6,2,3))
            # Different driver sizes shift IPL/startup clocks; compare tuning
            # semantics across images, not whole-run PCM fingerprints.
            self.assertLessEqual(abs(prior['pcm']['frames']-result['pcm']['frames']),1)
            for field in ('voice_pitch','voice_tuning','voice_profiles','prefix_ids','prefix_counts'):
                self.assertEqual(prior[field],result[field])

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0],*remaining])
