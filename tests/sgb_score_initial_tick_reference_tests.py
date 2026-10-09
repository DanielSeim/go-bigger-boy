#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact clipped-gate comparison guards with public register summaries."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from check_sgb_score_initial_tick_reference import compare,IMAGE_HASH


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
