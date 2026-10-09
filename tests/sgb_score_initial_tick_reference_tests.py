#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact asynchronous timing guards with public and synthetic summaries."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from check_sgb_score_initial_tick_reference import compare,IMAGE_HASH,PEER_IMAGE_HASH,CASES,peer_notes
from check_sgb_instrument_chromatic_reference import SETUPS,PITCHES
from check_sgb_instrument_envelope_reference import expected_points


def reports():
    # Sanitized register anchors; the model/orientation duplication below is
    # synthetic input for parser guards, not additional original measurements.
    points=dict(zip(map(str,(1,4,8,16,32,64,128,512,2048,4096)),(0,0,2,6,18,38,82,125,113,99)))
    references=[]
    runs=[]
    for model in ('sgb','sgb2'):
        for held in (2,3):
            envelopes=[]
            for voice in (2,3):
                last=92 if voice==held else 111
                envelopes.append({'voice':voice,'instrument':10,'base_note':24,'pitch':7993,'srcn':10,
                    'adsr1':142,'adsr2':175,'gain':184,'onset_spc_cycles':0,
                    'gate_spc_cycles':170900 if voice==held else 72953,'peak':127,'peak_sample_offset':197,
                    'active_last':last,'active_points':dict(points) if voice==held else {
                        key:value for key,value in points.items() if key!='4096'},
                    'release_start':last,'release_drops':last,'release_drop_samples':2,
                    'release_zero':True,'release_zero_spc_cycles':64*last+30})
            references.append({'model':model,'case':'clipped','held_voice':held,'envelopes':envelopes,
                               'held_setup_writes':0,'sample_count_per_voice':12000})
            for slots in ([2,3],[3,2]):
                runs.append({'model':model,'case':'clipped','mode':'native','held_voice':held,'slots':slots,
                    'version':0xD9,'restore_equal':True,'reset_equal':True,'restore_count':80,
                    'timing':[{'voice':voice,'note_index':0,'gate_spc_cycles':170538 if voice==held else 73725,
                               'onset_spc_cycles':0,'gate_matches':True,'onset_matches':True} for voice in (2,3)]})
    return {'schema':'gbb-sgb-score-initial-tick-v1','qualification':False,'playback':False,'suite_passed':True,
            'timing_qualified':True,'image_sha256':IMAGE_HASH,'reference_timing_allowance_spc_cycles':4096,
            'd8_control_runs':[],'runs':runs},references


def asynchronous_reports():
    """Synthetic contract-valid summaries; these are not reference measurements."""
    native,_=reports()
    native['runs']=[]
    references=[]
    template=reports()[0]['runs'][0]
    for case in CASES:
        for model in ('sgb','sgb2'):
            for held in (2,3):
                envelopes=[]
                for voice in (2,3):
                    sequence=((10,24),) if voice==held else peer_notes(case)
                    for index,(instrument,note) in enumerate(sequence):
                        shifted=voice!=held and ((case=='retrigger' and index==1) or (case=='switch' and index==2))
                        last=(92 if case=='clipped' else 70) if voice==held else (
                            (109 if index==3 else 110) if instrument==2 else 111)
                        profile=('short' if case=='clipped' else 'held') if voice==held else 'retrigger'
                        points=expected_points(10,'retrigger',25) if shifted else expected_points(
                            instrument,profile,24 if instrument==10 else 25)
                        envelopes.append({'voice':voice,'instrument':instrument,'base_note':note,
                            'pitch':PITCHES[instrument][note-24],
                            **dict(zip(('srcn','adsr1','adsr2','gain'),SETUPS[instrument])),
                            'onset_spc_cycles':0 if voice==held else
                                {0:0,16:84000,32:172000,48:258000}[index*(32 if case=='rests' else 16)],
                            'gate_spc_cycles':(170900 if case=='clipped' else 335000) if voice==held else
                                (72953 if index==0 else 76000 if index==3 else 73000),
                            'peak':127,'peak_sample_offset':10 if instrument==2 else 198 if shifted else 197,
                            'active_last':last,'active_points':points,'release_start':last,'release_drops':last,
                            'release_drop_samples':2,'release_zero':True,'release_zero_spc_cycles':64*last+30})
                references.append({'model':model,'case':case,'held_voice':held,'envelopes':envelopes,
                                   'held_setup_writes':0,'sample_count_per_voice':12000})
                for slots in ([2,3],[3,2]):
                    indices={2:0,3:0}
                    timings=[]
                    for note in envelopes:
                        voice=note['voice']
                        timings.append({'voice':voice,'note_index':indices[voice],
                            'gate_spc_cycles':note['gate_spc_cycles'],
                            'onset_spc_cycles':note['onset_spc_cycles'],'gate_matches':True,'onset_matches':True})
                        indices[voice]+=1
                    native['runs'].append(dict(template,case=case,model=model,held_voice=held,slots=slots,timing=timings))
    return native,references


class InitialTickReferenceTests(unittest.TestCase):
    def test_exact_gates_and_all_models_orientations_slots(self):
        native,originals=reports()
        comparisons=compare(native,originals)
        self.assertEqual(len(comparisons),16)
        self.assertEqual({row['difference_spc_cycles'] for row in comparisons},{-362,772})

    def test_native_malformed_types_duplicates_and_timestamp_mutations(self):
        native,originals=reports()
        variants=[dict(native,image_sha256='wrong'),dict(native,timing_qualified=False),
                  dict(native,qualification=True),dict(native,reference_timing_allowance_spc_cycles=8192),
                  dict(native,runs=native['runs'][:-1]),dict(native,runs=native['runs'][:-1]+[native['runs'][0]])]
        for key,value in (('version',217.0),('restore_count',True),('reset_equal',False),('held_voice',True),
                          ('slots',[2,2]),('case','held')):
            changed=copy.deepcopy(native)
            changed['runs'][0][key]=value
            variants.append(changed)
        for key,value in (('gate_spc_cycles',176700),('gate_spc_cycles',True),('gate_spc_cycles',float('nan')),
                          ('onset_spc_cycles',1),('gate_matches',False),('voice',True),('note_index',1)):
            changed=copy.deepcopy(native)
            changed['runs'][0]['timing'][0][key]=value
            variants.append(changed)
        for index,changed in enumerate(variants):
            with self.subTest(case=index),self.assertRaises(ValueError):
                compare(changed,originals)

    def test_all_cases_compare_every_onset_and_gate(self):
        native,originals=asynchronous_reports()
        result=compare(native,originals,CASES)
        self.assertEqual(len(result),120)
        self.assertEqual({row['case'] for row in result},set(CASES))
        self.assertTrue(all(row['difference_spc_cycles']==row['onset_difference_spc_cycles']==0 for row in result))

    def test_da_profile_requires_own_identity_and_empty_d9_controls(self):
        native,originals=asynchronous_reports()
        native.update(schema='gbb-sgb-score-peer-gate-v1',image_sha256=PEER_IMAGE_HASH,d9_control_runs=[])
        del native['d8_control_runs']
        for row in native['runs']:
            row['version']=0xDA
        self.assertEqual(len(compare(native,originals,CASES,True)),120)
        with self.assertRaises(ValueError):
            compare(native,originals,CASES)
        for key,value in (('image_sha256',IMAGE_HASH),('schema','gbb-sgb-score-initial-tick-v1'),
                          ('d9_control_runs',[{}])):
            with self.subTest(key=key),self.assertRaises(ValueError):
                compare(dict(native,**{key:value}),originals,CASES,True)

    def test_later_peer_edges_use_exact_allowance_without_window_width(self):
        native,originals=asynchronous_reports()
        # A later peer note has both a nonzero onset and a finite original window.
        timing=native['runs'][0]['timing'][2]
        self.assertEqual((timing['voice'],timing['note_index']),(3,1))
        for field in ('onset_spc_cycles','gate_spc_cycles'):
            for delta in (-4096,4096):
                changed=copy.deepcopy(native)
                changed['runs'][0]['timing'][2][field]+=delta
                compare(changed,originals,CASES)
                changed['runs'][0]['timing'][2][field]+=0.5 if delta>0 else -0.5
                with self.assertRaises(ValueError):
                    compare(changed,originals,CASES)
            for value in (True,float('nan'),float('inf')):
                changed=copy.deepcopy(native)
                changed['runs'][0]['timing'][2][field]=value
                with self.assertRaises(ValueError):
                    compare(changed,originals,CASES)

    def test_all_cases_require_complete_ordered_notes_and_matrix(self):
        native,originals=asynchronous_reports()
        for change in ('missing','duplicate','reordered','wrong_index','wrong_case'):
            changed=copy.deepcopy(native)
            notes=changed['runs'][0]['timing']
            if change=='missing':
                notes.pop()
            elif change=='duplicate':
                notes[-1]=copy.deepcopy(notes[-2])
            elif change=='reordered':
                notes.reverse()
            elif change=='wrong_index':
                notes[2]['note_index']=True
            else:
                changed['runs'][0]['case']='switch'
            with self.subTest(change=change),self.assertRaises(ValueError):
                compare(changed,originals,CASES)
        for refs in (originals[:-1],originals[:-1]+[originals[0]]):
            with self.assertRaises(ValueError):
                compare(native,refs,CASES)

    def test_original_trajectory_and_complete_matrix_required(self):
        native,originals=reports()
        variants=[originals[:-1],originals[:-1]+[originals[0]]]
        for key,value in (('peak',126),('release_zero',False),('gate_spc_cycles',176700),('srcn',2)):
            changed=copy.deepcopy(originals)
            changed[0]['envelopes'][0][key]=value
            variants.append(changed)
        for changed in variants:
            with self.assertRaises(ValueError):
                compare(native,changed)


if __name__=='__main__':
    unittest.main()
