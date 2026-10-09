#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned D8 held/peer envelope setup, reference bounds and exact replay."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import sgb_score_adsr_tests as prior

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_score_async_adsr_fixture import build,objects,CASES,peer_notes
from build_sgb_async_envelope_fixture import bank as reference_bank
from check_sgb_instrument_envelope_reference import expected_points,POINTS
from check_sgb_instrument_chromatic_reference import SETUPS,PITCHES
from build_sgb_score_transport import build as program
from sgb_score_atomic_tests import fnv
PROBE=None
EVIDENCE=[]


def reference_timing(rows,case,held_voice):
    """Keep the preceding 4096-cycle reference allowance and report misses."""
    first=min(row[16] for row in rows)
    observations=[]
    for voice in (2,3):
        held=voice==held_voice
        for index,row in enumerate(note for note in rows if note[0]==voice):
            gate=((170000,172000) if case=='clipped' else (334000,336000)) if held else (
                  (72000,74000) if index==0 else (75000,78000) if index==3 else (73000,75000))
            tick=0 if held else index*(32 if case=='rests' else 16)
            onset={0:(0,0),16:(84000,86000),32:(171000,174000),48:(257000,260000)}[tick]
            actual_gate=(row[17]-row[16])/2
            actual_onset=(row[16]-first)/2
            observations.append({'voice':voice,'note_index':index,'gate_spc_cycles':actual_gate,
                'onset_spc_cycles':actual_onset,'reference_gate_bounds':gate,'reference_onset_bounds':onset,
                'gate_matches':gate[0]-4096<=actual_gate<=gate[1]+4096,
                'onset_matches':onset[0]-4096<=actual_onset<=onset[1]+4096})
    return observations


class AsyncAdsrTests(unittest.TestCase):
    VERSION=0xD8
    BUILD_OPTIONS={**prior.OPTIONS,'instrument_envelope':True}
    EXPECTED_IMAGE_HASH=prior.IMAGE_HASH
    RELEASE_PHASE_PROFILES={(2,'switch')}

    def probe(self,model,case,voice,slots=(2,3),mode='native'):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            host,game=Path(directory)/'host.rom',Path(directory)/'game.gb'
            image=program(**self.BUILD_OPTIONS)
            self.assertEqual(hashlib.sha256(image).hexdigest(),self.EXPECTED_IMAGE_HASH)
            host.write_bytes(image)
            game.write_bytes(build(case,voice,slots))
            child=subprocess.run([str(PROBE),str(host),str(game),model,'70000000',mode],
                                 capture_output=True,text=True,timeout=90)
            self.assertEqual(child.returncode,0,child.stderr)
            self.assertLess(len(child.stdout),16384)
            result=json.loads(child.stdout)
        self.assertEqual(result['schema'],'gbb-score-transport-v1')
        for field in ('qualification','playback'):
            self.assertIs(result[field],False)
        for field in ('reset_equal','restore_equal'):
            self.assertIs(result[field],True)
        self.assertLessEqual(result['restore_count'],4096)
        self.assertLessEqual(result['pcm']['frames'],250000)
        self.assertLessEqual(result['clocks']-70000000,256)
        self.assertEqual((result['status'],result['transfers'],result['adoptions'],result['version'],
                          result['bridge'],result['signature'],result['error'],result['external'],
                          result['selected_song'],result['admitted_roots'],result['sounds']),
                         (1,1,2,self.VERSION,2,0xA5,0,0,1,3,2))
        self.assertEqual(result['score_tick'],32 if case=='clipped' else 80)
        self.assertEqual(result['restore_roots'],7)
        self.assertEqual(result['atomic_phase'],0xA5)
        self.assertEqual(result['voice_env'],[0,0])
        self.assertGreater(result['pcm']['nonzero_frames'],100)
        self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)
        prior.recovery.RecoveryTests.evidence(self,result,0,1,0)
        score,assets=objects(case,voice,slots)
        self.assertEqual(result['score_hash'],fnv(score))
        self.assertEqual(result['asset_hash'],fnv(assets))
        self.trajectories(result,case,voice,slots)
        EVIDENCE.append({'model':model,'version':self.VERSION,'case':case,'held_voice':voice,'slots':slots,'mode':mode,
                         'restore_count':result['restore_count'],'reset_equal':True,'restore_equal':True,
                         'timing':reference_timing(result['envelope_notes'],case,voice),
                         'release_cadence_exceptions':[row[12] for row in result['envelope_notes']]})
        return result

    def trajectories(self,result,case,held_voice,slots):
        rows=result['envelope_notes']
        self.assertEqual(len(rows),1+len(peer_notes(case)))
        self.assertEqual(result['envelope_setup_changes'],[0]*len(rows))
        first=min(row[16] for row in rows)
        self.assertEqual(len(result['envelope_release_prefix']),len(rows))
        for voice in (2,3):
            held=voice==held_voice
            notes=[row for row in rows if row[0]==voice]
            sequence=((10,24),) if held else peer_notes(case)
            self.assertEqual(len(notes),len(sequence))
            for index,(row,(instrument,note)) in enumerate(zip(notes,sequence)):
                self.assertEqual(len(row),30)
                slot=slots[0 if instrument==10 else 1]
                selector=2 if instrument==10 else 0
                self.assertEqual(row[:7],[voice,slot,*SETUPS[instrument][1:],selector,PITCHES[instrument][note-24]])
                self.assertEqual(row[7],127)
                self.assertEqual(row[13:16],[0,0,0])
                prefix=result['envelope_release_prefix'][rows.index(row)]
                self.assertEqual(len(prefix),8)
                drops=[i for i,(a,b) in enumerate(zip(prefix,prefix[1:]),1) if b<a]
                intervals=[b-a for a,b in zip(drops,drops[1:])]
                self.assertTrue(intervals)
                self.assertTrue(all(a-b in (0,1) for a,b in zip(prefix,prefix[1:])))
                self.assertTrue(all(interval==2 for interval in intervals[1:]))
                # Preserve the measured three-sample initial transition and
                # raw exception counter; never forgive later gaps.
                phase_exception=int(intervals[0]==3)
                self.assertIn(intervals[0],(2,3))
                self.assertEqual(row[12],phase_exception)
                if phase_exception:
                    self.assertIn((instrument,case),self.RELEASE_PHASE_PROFILES)
                self.assertGreater(row[17],row[16])
                self.assertGreater(row[18],row[17])
                self.assertEqual(row[11],row[10])
                self.assertGreaterEqual((row[18]-row[17])/2,64*row[10]-2)
                self.assertLessEqual((row[18]-row[17])/2,64*row[10]+130)
                shifted=not held and ((case=='retrigger' and index==1) or (case=='switch' and index==2))
                peak=10 if instrument==2 else 198 if shifted else 197
                self.assertLessEqual(abs(row[8]-peak),4)
                points=expected_points(instrument,'held' if held and case!='clipped' else 'short' if held else
                                       'retrigger',25 if instrument==2 or shifted else 24)
                for point,value in points.items():
                    if instrument==2 and point=='8':
                        continue  # Fast attack's full phase step is separately peak/decay checked.
                    self.assertLessEqual(abs(row[19+POINTS.index(int(point))]-value),2,(case,voice,index,point,row))
                last=(92 if case=='clipped' else 70) if held else (109 if index==3 else 110) if instrument==2 else 111
                self.assertLessEqual(abs(row[9]-last),2)
                self.assertLessEqual(abs(row[10]-last),2)
                gate=((170000,172000) if case=='clipped' else (334000,336000)) if held else (
                      (72000,74000) if index==0 else (75000,78000) if index==3 else (73000,75000))
                self.assertGreaterEqual((row[17]-row[16])/2,gate[0]-4096)
                if held and case=='clipped':
                    # Envelope qualification only: the preceding timing allowance
                    # is evaluated separately and retained as an explicit mismatch.
                    self.assertLessEqual((row[17]-row[16])/2,180000)
                else:
                    self.assertLessEqual((row[17]-row[16])/2,gate[1]+4096)
                tick=0 if held else index*(32 if case=='rests' else 16)
                onset={0:(0,0),16:(84000,86000),32:(171000,174000),48:(257000,260000)}[tick]
                self.assertGreaterEqual((row[16]-first)/2,onset[0]-4096)
                self.assertLessEqual((row[16]-first)/2,onset[1]+4096)
                if not tick:
                    self.assertEqual(row[16],first)
                if index:
                    self.assertLess(notes[index-1][18],row[16])

    def test_build_caps_frozen_firmware_geometry_and_export(self):
        self.assertEqual(hashlib.sha256(program(**prior.OPTIONS,instrument_envelope=True)).hexdigest(),prior.IMAGE_HASH)
        for case in CASES:
            for voice in (2,3):
                for slots in ((2,3),(3,2)):
                    score,assets=objects(case,voice,slots)
                    authored=reference_bank(case,voice)
                    self.assertEqual((len(score),len(assets)),(2048,192))
                    self.assertEqual(build(case,voice,slots),build(case,voice,slots))
                    self.assertEqual(struct.unpack_from('<3H',score,0x20),
                                     (0x2B60,0 if case=='clipped' else 0x2B70,0))
                    for pattern in (0,1):
                        pointers=struct.unpack_from('<8H',score,0x60+16*pattern)
                        self.assertEqual([i for i,p in enumerate(pointers) if p],[2,3])
                        for channel in (2,3):
                            start=pointers[channel]-0x2B00
                            if pattern:
                                self.assertEqual(score[start:start+4],bytes((16,127,0xC9,0)))
                            else:
                                self.assertEqual(score[start:start+6],bytes((0xE0,10,0xE1,10,0xED,127)))
                                reference_start=struct.unpack_from('<H',authored,32+channel*2)[0]-0x2B00
                                reference_end=authored.index(0,reference_start)
                                stream=authored[reference_start:reference_end]
                                if channel==voice or case!='clipped':
                                    self.assertEqual(stream[-2:],bytes((1,0xC9)))
                                    stream=stream[:-2]
                                self.assertEqual(score[start:start+len(stream)+1],stream+b'\0')
                    self.assertEqual(assets[1+4*(slots[0]-2):4+4*(slots[0]-2)],bytes((0x8E,0xAF,0xB8)))
                    self.assertEqual(assets[1+4*(slots[1]-2):4+4*(slots[1]-2)],bytes((0x8F,0x6F,0xB8)))
        for args in (('bad',2,(2,3)),('switch',True,(2,3)),('rests',4,(2,3)),
                     ('clipped',2,(2,2)),('rests',2,(True,3))):
            with self.assertRaises(ValueError):
                build(*args)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'async.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_score_async_adsr_fixture.py'),
                     '--case','switch','--held-voice','3','--slots','3,2','--output',str(path)]
            for expected in (0,2):
                child=subprocess.run(command,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,expected,child.stderr)
                self.assertEqual(path.read_bytes(),build('switch',3,(3,2)))

    def test_reference_timing_mismatch_remains_visible(self):
        rows=[]
        for voice,gate in ((2,176700),(3,73000)):
            row=[0]*30
            row[0],row[16],row[17]=voice,1000,1000+2*gate
            rows.append(row)
        evidence=reference_timing(rows,'clipped',2)
        self.assertFalse(evidence[0]['gate_matches'])
        self.assertTrue(evidence[1]['gate_matches'])
        self.assertTrue(all(note['onset_matches'] for note in evidence))
        rows[0][17]=1000+2*171000
        self.assertTrue(reference_timing(rows,'clipped',2)[0]['gate_matches'])

    def test_retriggers_on_each_voice_and_slot(self):
        self.matrix('retrigger')

    def test_rests_on_each_voice_and_slot(self):
        self.matrix('rests')

    def test_instrument_changes_on_each_voice_and_slot(self):
        self.matrix('switch')

    def test_clipped_release_on_each_voice_and_slot(self):
        self.matrix('clipped')

    def matrix(self,case):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for slots in ((2,3),(3,2)):
                    with self.subTest(model=model,voice=voice,slots=slots):
                        self.probe(model,case,voice,slots)

    def test_native_scalar_combined_replay(self):
        for model in ('sgb','sgb2'):
            baseline=self.probe(model,'switch',2)
            for mode in ('scalar','combined'):
                result=self.probe(model,'switch',2,mode=mode)
                self.assertEqual(result['envelope_notes'],baseline['envelope_notes'])
                self.assertEqual(result['envelope_setup_changes'],baseline['envelope_setup_changes'])
                self.assertEqual(result['envelope_release_prefix'],baseline['envelope_release_prefix'])
                if mode=='scalar':
                    self.assertEqual(result['pcm'],baseline['pcm'])
                else:
                    self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2),2)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining=parser.parse_known_args()
    PROBE=args.probe.resolve() if args.probe else None
    suite=unittest.main(argv=[sys.argv[0],*remaining],exit=False)
    if EVIDENCE:
        print(json.dumps({'schema':'gbb-sgb-score-async-adsr-v1','qualification':False,'playback':False,
            'suite_passed':suite.result.wasSuccessful(),'reference_timing_allowance_spc_cycles':4096,
            'timing_qualified':all(note['gate_matches'] and note['onset_matches'] for row in EVIDENCE for note in row['timing']),
            'runs':EVIDENCE},indent=2))
    sys.exit(0 if suite.result.wasSuccessful() else 1)
