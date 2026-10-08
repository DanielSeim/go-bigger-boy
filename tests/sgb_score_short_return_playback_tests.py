#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualified short gates, provisional-cache safety and physical lifecycle."""
import argparse,copy,hashlib
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_short_return import build,source
from build_sgb_score_direct_return import build as prior_build
from build_sgb_score_short_return_fixture import bank,CASES,ARTICULATIONS,DURATIONS
from build_sgb_score_direct_return_fixture import bank as prior_bank
from check_sgb_score_short_return_playback import native,validate,identity
from check_sgb_score_direct_return_playback import validate as prior_validate,SCHEMA as PRIOR_SCHEMA
from check_sgb_score_final_playback import observation,control_writes
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
PROBE=None


class ShortReturnPlaybackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{a}-{d}':native(PROBE,bank(a,d,c)) for c in CASES for a in ARTICULATIONS for d in DURATIONS}

    def test_reproducibility_bounds_predecessor_and_unchanged_general_profiles(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4089)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'d86d1587cb4329b9ded58e42a14d589b66af92fca370badcf3e78c2966139ba0')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'11eba7bf28920d59fd0278de5149263fc6803328554d4c030f8cd2e2f998440a')
        self.assertNotIn((96,63,4),PULSES);self.assertNotIn((96,127,4),PULSES)
        for r in self.reports.values():
            c,a,d=identity(r);self.assertEqual(r['short_return_mode'],1);self.assertEqual(r['direct_return_mode'],1)
            self.assertEqual(r['end_tick'],68);self.assertEqual(r['events'][6]['duration'],4)
            self.assertEqual(r['events'][-1]['duration'],d)

    def test_short_gate_replaces_the_frozen_voice_without_inventing_release(self):
        for r in self.reports.values():
            old,new=r['envelopes'][2],r['envelopes'][6]
            self.assertGreater(r['frozen_peer_checks'][0],300000)
            self.assertEqual(old['off_half_cycle'],0);self.assertFalse(old['release_zero'])
            self.assertEqual(old['retrigger_half_cycle'],new['on_half_cycle'])
            self.assertEqual(r['keyons'][4]['pending_pulses'][0],5)
            self.assertTrue(new['attack_zero']);self.assertEqual(new['peak'],127);self.assertTrue(new['release_zero'])
            self.assertLess(new['off_half_cycle'],control_writes(r)[0]['half_cycle'])
            gates,held=observation(r,allow_retrigger=True)
            self.assertEqual([(g['onset_index'],g['voice']) for g in gates],[(0,2),(0,3),(1,3),(2,3),(3,3),(4,2)])
            self.assertTrue(all(g['cause']==1 for g in gates));self.assertLessEqual(abs(gates[-1]['gate_spc_cycles']-5*2048),2048)

    def test_exact_setup_and_return_control_sequence(self):
        for r in self.reports.values():
            lo,hi=(r['events'][i]['half_cycle'] for i in (6,7))
            ws=[w for w in r['voice_writes'] if lo<=w['half_cycle']<hi]
            self.assertEqual([(w['address'],w['value']) for w in ws],[(34,164),(35,6),(32,7),(33,7)])
            kon=r['keyons'][4]['half_cycle'];ws=[w for w in r['key_writes'] if w['half_cycle']<=kon][-3:]
            self.assertEqual([(w['address'],w['value']) for w in ws],[(92,0),(92,0),(76,4)])
            self.assertEqual(r['keyons'][4]['held_mask'],0)

    def test_known_pending_durations_and_final_audio_reset_save_load(self):
        for r in self.reports.values():
            c,a,d=identity(r);self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertTrue(r['source_unmodified']);self.assertTrue(r['cache_guards_equal'])
            self.assertEqual(r['final_observation_half_cycles'],600000)
            self.assertEqual([(w['address'],w['value']) for w in control_writes(r)],[(92,255),(92,0),(76,4 if c=='direct-note' else 0)])
            if c=='direct-note':
                self.assertEqual(r['keyons'][-1]['pitches'][0],1800);self.assertEqual(r['keyons'][-1]['volumes'][0],[7,7])
                self.assertGreater(r['final_tail_pcm']['nonzero_frames'],0);self.assertGreater(r['final_env_end'],0)
            else:self.assertEqual(r['final_tail_pcm']['nonzero_frames'],0);self.assertEqual(r['final_env_end'],0)

    def test_provisional_short_profile_requires_full_rehearsal_qualification(self):
        mutations=[]
        for offset,value in ((0x6BD,8),(0x6DF,0xA1),(0x6E4,0xA0),(0x6E2,4),(0x2FC,63),(0x2FB,4),(0x6DE,127)):
            b=bytearray(bank());b[offset]=value;mutations.append(bytes(b))
        # Continuing channel 3 prevents the measured immediate final boundary.
        b=bytearray(bank());b[0x6C2:0x6C6]=bytes((8,63,0xC9,0));mutations.append(bytes(b))
        for b in mutations:
            r=native(PROBE,b);self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[])
            self.assertEqual(r['pcm']['nonzero_frames'],0)

    def test_predecessor_rejects_short_banks_and_previous_direct_profiles_survive(self):
        for c in CASES:
            r=gate_native(PROBE,bank(case=c),builder=prior_build,validator=validate,output_bound=131072)
            self.assertEqual(r['status'],226)
            r=gate_native(PROBE,prior_bank(case=c),builder=build,validator=lambda r:prior_validate({**r,'schema':PRIOR_SCHEMA}),output_bound=131072)
            self.assertEqual(r['short_return_mode'],0)

    def test_profile_counter_retrigger_order_and_freeze_faults_fail(self):
        for before,after in (('    mov $76, #$05\n','    mov $76, #$0f\n'),
                             ('    call direct_return_voice\n','    call pending_mix\n    call duet_voice\n'),
                             ('    mov $c4, #$01\n','    mov $c4, #$00\n'),
                             ('reselect_pulse2:\n    mov a, $a3\n    and a, #$04\n    beq gates_pulse3\n','reselect_pulse2:\n')):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_short_flag_raw_profile_envelope_and_audio_forgeries_fail(self):
        for mutation in ('mode','duration','pulses','retrigger','attack','release','tail','restore'):
            r=copy.deepcopy(self.reports['direct-note-63-8'])
            if mutation=='mode':r['short_return_mode']=0
            elif mutation=='duration':r['events'][6]['duration']=8
            elif mutation=='pulses':r['keyons'][4]['pending_pulses'][0]=4
            elif mutation=='retrigger':r['envelopes'][2]['retrigger_half_cycle']=0
            elif mutation=='attack':r['envelopes'][6]['attack_zero']=False
            elif mutation=='release':r['keyoffs'][-1]['cause']=0
            elif mutation=='tail':r['final_tail_pcm']['nonzero_frames']=0
            elif mutation=='restore':r['restore_equal']=False
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)

    def test_missing_final_FF_or_invented_inactive_release_fails_raw_ledger(self):
        original=self.reports['direct-note-63-8'];r=copy.deepcopy(original)
        control_writes(r)[0]['value']=4
        with self.assertRaises(ValueError):validate(r)
        r=copy.deepcopy(original);fake=copy.deepcopy(r['keyoffs'][-1]);fake.update(half_cycle=control_writes(r)[0]['half_cycle'],mask=8,held_mask=255,cause=0)
        r['keyoffs'].append(fake)
        with self.assertRaises(ValueError):validate(r)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
