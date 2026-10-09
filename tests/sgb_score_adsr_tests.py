#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact ADSR descriptors, envelope trajectories and physical upload lifecycle."""
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
from build_sgb_score_adsr_fixture import build as cartridge, objects, FAULTS, ENVELOPES
from check_sgb_instrument_envelope_reference import expected_points, POINTS, LAST, GATES
from check_sgb_instrument_chromatic_reference import PITCHES
from build_sgb_score_instrument_profiles_fixture import PROFILES
PROBE = None
OPTIONS = {**mapping.OPTIONS,'instrument_mapping':True,'instrument_tuning':True}
IMAGE_HASH = '8cb99b32c080368a0573a7a0c125522e9d5940ef0a4cd59a51ab9971993c47c0'


def pitch(note, selector):
    return PITCHES[10][note-24] if selector == 2 else PITCHES[2][note-24] >> selector


class AdsrTests(unittest.TestCase):
    VERSION=0xD8
    EXTRA_OPTIONS={}

    def probe(self, model, order=(1,), clocks=45000000, mode='native', envelope_image=True, **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom,game = Path(directory)/'host.rom',Path(directory)/'game.gb'
            rom.write_bytes(program(**OPTIONS,instrument_envelope=envelope_image,**self.EXTRA_OPTIONS))
            game.write_bytes(cartridge(order,**fixture))
            child = subprocess.run([str(PROBE),str(rom),str(game),model,str(clocks),mode],
                                   capture_output=True,text=True,timeout=90)
            self.assertEqual(child.returncode,0,child.stderr)
            self.assertLess(len(child.stdout),16384)
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
              attempts=0, suppressed=0, events=None, case=None):
        self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                          result['version'],result['bridge'],result['signature'],
                          result['error'],result['external'],result['selected_song'],
                          result['admitted_roots'],result['sounds']),
                         (1,transfers,transfers+1 if adoptions is None else adoptions,
                          self.VERSION,2,0xA5,0,0,song,3,sounds))
        self.assertEqual(result['score_tick'],48 if case == 'retrigger' else 80 if case else 76 if song == 3 else 72)
        self.assertEqual(result['restore_roots'],7)
        self.assertEqual(result['atomic_phase'],0xA5)
        self.assertEqual(result['voice_env'],[0,0])
        self.assertGreater(result['pcm']['nonzero_frames'],100)
        self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)
        recovery.RecoveryTests.evidence(self,result,attempts,transfers if events is None else events,suppressed)

    def voices(self, result, sources, tuning=(2,0), notes=(33,27), envelope='both'):
        self.assertEqual(result['voice_profiles'],[field for slot in sources for field in
                         (slot,*ENVELOPES[envelope][slot-2])])
        selectors = [tuning[slot-2] for slot in sources]
        self.assertEqual(result['voice_tuning'],selectors)
        self.assertEqual(result['voice_pitch'],[pitch(note,selector) for note,selector in zip(notes,selectors)])

    def trajectories(self,result,case,slots=(2,3),envelope='both',tuning=(2,0)):
        notes = (24,25) if case == 'retrigger' else (24,)
        self.assertEqual(len(result['envelope_notes']),2*len(notes))
        for i,row in enumerate(result['envelope_notes']):
            self.assertEqual(len(row),30)
            voice,slot,note = 2+i%2,slots[i%2],notes[i//2]
            descriptor=ENVELOPES[envelope][slot-2]
            self.assertEqual(row[:7],[voice,slot,*descriptor,tuning[slot-2],pitch(note,tuning[slot-2])])
            self.assertEqual(row[7],127)
            self.assertEqual(row[12:16],[0,0,0,0]) # cadence, decay, release, missed samples
            self.assertGreater(row[17],row[16])
            self.assertGreater(row[18],row[17])
            self.assertEqual(row[11],row[10])
            release=(row[18]-row[17])/2
            self.assertGreaterEqual(release,64*row[10]-2)
            self.assertLessEqual(release,64*row[10]+130)
            low,high=GATES[case]
            self.assertGreaterEqual((row[17]-row[16])/2,low-4096)
            self.assertLessEqual((row[17]-row[16])/2,high+4096)
            instrument=10 if descriptor==(0x8E,0xAF,0xB8) else 2
            reference_peak=(197 if note==24 else 198) if instrument==10 else (9 if note==24 else 10)
            self.assertLessEqual(abs(row[8]-reference_peak),4)
            for point,value in expected_points(instrument,case,note).items():
                # Fast attack can cross a whole step at sample 8 when the KON
                # latch phase differs. Slow attack and all later anchors stay bounded.
                if instrument==2 and int(point)==8:
                    continue
                actual=row[19+POINTS.index(int(point))]
                self.assertLessEqual(abs(actual-value),2,(case,voice,note,point,actual,value))
            self.assertLessEqual(abs(row[9]-LAST[instrument][case]),2)

    def test_caps_and_frozen_images_and_exports(self):
        image=program(**OPTIONS,instrument_envelope=True)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),IMAGE_HASH)
        self.assertEqual(hashlib.sha256(program(**OPTIONS)).hexdigest(),
                         '61cac6e3c1dbcfe40318714a6044bd68bc65170302e6c0fd3c64b0570efd8fdb')
        for flags in ({'instrument_envelope':True},{**OPTIONS,'instrument_envelope':1}):
            with self.assertRaises(ValueError): program(**flags)
        for flags in ({'envelope_profile':'unknown'},{'replacement_envelope':'first'},
                      {'fault':'unknown'},{'tuning':(2,3)},{'octave_slots':(True,3)}):
            with self.assertRaises(ValueError): cartridge(**flags)
        with tempfile.TemporaryDirectory() as directory:
            for i,(script,flags,expected) in enumerate((
                    ('build_sgb_score_transport.py',['--'+k.replace('_','-') for k in (*OPTIONS,'instrument_envelope')],image),
                    ('build_sgb_score_adsr_fixture.py',['--score-profile','held','--envelope-profile','first','--tuning','1,2'],
                     cartridge(score_profile='held',envelope_profile='first',tuning=(1,2))),
                    ('build_sgb_score_adsr_fixture.py',['--fault','decay2','--recover','--repeat-failure','--chunking','tail',
                     '--replacement-envelope','second'],cartridge(fault='decay2',recover=True,repeat_failure=True,
                     chunking='tail',replacement_envelope='second')))):
                path=Path(directory)/f'{i}.bin'
                cmd=[sys.executable,str(ROOT/'scripts'/script),*flags,'--output',str(path)]
                child=subprocess.run(cmd,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,0,child.stderr)
                self.assertEqual(path.read_bytes(),expected)
                self.assertNotEqual(subprocess.run(cmd,capture_output=True,timeout=10).returncode,0)
                self.assertEqual(path.read_bytes(),expected)

    def test_measured_held_short_and_retrigger_shapes(self):
        for model in ('sgb','sgb2'):
            for case in ('held','short','retrigger'):
                result=self.probe(model,score_profile=case,clocks=70000000,tuning=(2,2))
                self.ready(result,case=case)
                self.regions(result,score_profile=case,tuning=(2,2))
                self.trajectories(result,case,tuning=(2,2))

    def test_each_slot_voice_and_tuning_independence(self):
        for model in ('sgb','sgb2'):
            for envelope,tuning in (('first',(1,2)),('second',(2,0))):
                result=self.probe(model,score_profile='held',clocks=70000000,envelope_profile=envelope,
                                  tuning=tuning,octave_slots=(3,2),ids=(10,2))
                self.ready(result,case='held')
                self.regions(result,(10,2),score_profile='held',envelope_profile=envelope,
                             tuning=tuning,octave_slots=(3,2))
                self.trajectories(result,'held',(3,2),envelope,tuning)

    def test_native_scalar_and_combined_render_modes(self):
        for model in ('sgb','sgb2'):
            baseline=None
            for mode in ('native','scalar','combined'):
                result=self.probe(model,score_profile='retrigger',clocks=70000000,mode=mode,tuning=(2,2))
                self.ready(result,case='retrigger')
                self.trajectories(result,'retrigger',tuning=(2,2))
                if baseline is None: baseline=result
                elif mode=='scalar':
                    self.assertEqual(result['pcm'],baseline['pcm'])
                    self.assertEqual(result['envelope_notes'],baseline['envelope_notes'])
                else:
                    self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2),2)
                    self.assertEqual(result['envelope_notes'],baseline['envelope_notes'])

    def test_inheritance_prefixes_executed_ends_and_clipped_tails(self):
        for model in ('sgb','sgb2'):
            for score,sources in (('switch',(3,2)),('inherit',(3,3)),('default',(3,2)),
                    ('repeat',(3,2)),('maximum',(3,2)),('end',(3,3)),('clipped',(3,2))):
                result=self.probe(model,score_profile=score,envelope_profile='first')
                self.ready(result)
                self.regions(result,score_profile=score,envelope_profile='first')
                self.voices(result,sources,envelope='first')
                if score=='maximum': self.assertEqual(result['prefix_counts'][:4],[26,1,27,1])
                if score=='default':
                    self.assertEqual(result['envelope_notes'][0][1:5],[2,0x8E,0xAF,0xB8])
                    self.assertEqual(result['envelope_notes'][1][1:5],[2,0x8E,0xAF,0xB8])

    def test_active_replacement_stop_and_reupload(self):
        for model in ('sgb','sgb2'):
            for stop in (False,True):
                result=self.probe(model,(1,3),replace=True,stop=stop,clocks=100000000,
                    score_profile='all2',replacement_score_profile='switch',envelope_profile='first',
                    replacement_envelope='second',replacement_tuning=(1,2))
                self.ready(result,3,transfers=2)
                self.regions(result,(10,2),score_profile='switch',envelope_profile='second',tuning=(1,2))
                self.voices(result,(3,2),(1,2),envelope='second')
                self.assertEqual(result['interruptions'],1 if stop else 4)
                self.assertEqual(result['active_env'],result['interruptions'])
            result=self.probe(model,(3,1,2),repeat_upload=True,active=True,clocks=125000000)
            self.ready(result,2,transfers=3)
            self.voices(result,(3,2),notes=(32,27))
            self.regions(result)

    def test_silent_rejection_of_cross_pairs_gain_and_unmeasured_gate(self):
        for model in ('sgb','sgb2'):
            for fault in ('decay2','decay3','attack2','attack3','gain2','gain3','off2','off3','reserved','gate-tempo'):
                result=self.probe(model,fault=fault,score_profile='held' if fault=='gate-tempo' else 'switch')
                self.assertEqual((result['status'],result['version'],result['bridge'],result['admitted_roots']),
                                 (9,0xD8,0xE2,0))
                self.assertEqual(result['pcm']['nonzero_frames'],0)
                self.assertEqual(result['envelope_notes'],[])
                self.assertEqual(result['restore_roots'],0)
                self.regions(result,fault=fault,score_profile='held' if fault=='gate-tempo' else 'switch')
                recovery.RecoveryTests.evidence(self,result,1,1,1,blocked=1)

    def test_fragmented_cold_and_active_rejection_recovery(self):
        for model in ('sgb','sgb2'):
            for warm,fault,chunk in ((False,'decay3','tail'),(True,'unknown-third','interleave')):
                result=self.probe(model,fault=fault,recover=True,active_failure=warm,repeat_failure=True,
                    stop=True,chunking=chunk,clocks=100000000,score_profile='all2',
                    replacement_score_profile='switch',replacement_envelope='second',replacement_tuning=(0,2))
                self.ready(result,3 if warm else 1,transfers=3+int(warm),adoptions=2+int(warm),
                           attempts=2,suppressed=3,events=3+int(warm))
                self.regions(result,(10,2),score_profile='switch',envelope_profile='second',tuning=(0,2))
                self.voices(result,(3,2),(0,2),envelope='second')

    def test_frozen_d7_rejects_new_pair_and_renders_legacy(self):
        for model in ('sgb','sgb2'):
            result=self.probe(model,envelope_image=False)
            self.assertEqual((result['status'],result['version'],result['bridge'],result['admitted_roots']),
                             (9,0xD7,0xE2,0))
            self.assertEqual(result['pcm']['nonzero_frames'],0)
            result=self.probe(model,envelope_profile='legacy')
            self.ready(result)
            self.voices(result,(3,2),envelope='legacy')
            prior=self.probe(model,envelope_image=False,envelope_profile='legacy')
            self.assertEqual((prior['status'],prior['version'],prior['bridge'],prior['admitted_roots']),
                             (1,0xD7,2,3))
            for field in ('voice_pitch','voice_tuning','voice_profiles','prefix_ids','prefix_counts'):
                self.assertEqual(result[field],prior[field])

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining=parser.parse_known_args()
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0],*remaining])
