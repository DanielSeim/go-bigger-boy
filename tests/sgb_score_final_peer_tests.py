#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical final peer release, bounded opaque evidence and restoration guards."""
import argparse,copy,hashlib,io,json
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_final_peer import build,source
from build_sgb_score_final import build as prior_build
from build_sgb_score_final_peer_fixture import bank,build as fixture_build,expected,CASES
from check_sgb_score_final_peer_reference import observe,contract,agree
from check_sgb_score_final_peer_playback import native,validate,align,control_writes,observation
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
from build_sgb_sparse_fixture import bank as sparse_bank
PROBE=None


def trace(case,art):
    events=[(0,61,0)];ending=int(case[-1]);peer=5-ending
    for i,e in enumerate(expected(case)):
        c=10000+i*88000
        for v in e['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            events.extend((c,16*v['voice']+r,x) for r,x in enumerate(values))
            if not(i==1 and v['voice']==peer and art==127):
                d=24 if i==1 and v['voice']==peer else 16
                events.append((c+PULSES[(96,art,d)]*2048,92,1<<v['voice']))
        events.append((c,76,e['mask']))
    stop=10000+88000*len(expected(case))
    events.extend(((stop,92,255),(stop+117,92,0),(stop+156,76,0),
                   (stop+2048,92,0),(stop+2165,92,0),(stop+2204,76,0),(stop+400000,61,0)))
    events.sort(key=lambda e:e[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+''.join(f'D,0,{c},0,{r},{v}\n' for c,r,v in events)


def references():
    result=[]
    for c in CASES:
        for a in (63,127):
            r=observe(io.StringIO(trace(c,a)),c,a)
            result.extend({**copy.deepcopy(r),'case':c,'model':m} for m in ('sgb','sgb2'))
    return result


class FinalPeerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{a}':native(PROBE,bank(a,c)) for c in CASES for a in (63,127)}

    def test_reproducible_owned_banks_and_preserved_prior_image(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4089)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'09c6b3f1b641cd232f936a68b507be239a789766e7b7ec60f606556710db36e5')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'00ff6c59ecdb5915b9cb2e742b45b17bb1422438150923c0be2cc6877d7ddf7b')
        hashes=set()
        for c in CASES:
            for a in (63,127):
                data=bank(a,c);self.assertEqual(len(data),2048)
                self.assertEqual(data[0x103 if c.startswith('inactive') else 0x101:0x105 if c.startswith('inactive') else 0x103],bytes(2))
                align(self.reports[f'{c}-{a}'],data)
                hashes.add(hashlib.sha256(fixture_build(a,c)).hexdigest())
        self.assertEqual(len(hashes),8)

    def test_final_pulse_releases_only_a_still_sounding_peer(self):
        for c in CASES:
            for a in (63,127):
                r=self.reports[f'{c}-{a}'];gates,held=observation(r)
                self.assertEqual(held,[])
                stops=[g for g in gates if g['cause']==0]
                self.assertEqual([g['voice'] for g in stops],[5-int(c[-1])] if a==127 else [])
                self.assertTrue(all(g['onset_index']==1 for g in stops))
                ws=control_writes(r);self.assertEqual([(w['address'],w['value']) for w in ws],[(92,255),(92,0),(76,0)])
                for e in r['keyoffs']:
                    if e['cause']==0:self.assertEqual((e['half_cycle'],e['held_mask']),(ws[0]['half_cycle'],255))

    def test_inactive_voice_is_frozen_until_the_stop_and_never_retriggered(self):
        for c in ('inactive-2','inactive-3'):
            r=self.reports[f'{c}-127'];peer=5-int(c[-1])
            self.assertGreater(r['frozen_peer_checks'][peer-2],300000)
            self.assertEqual([e['mask'] for e in r['keyons']],[12,12,1<<int(c[-1]),1<<int(c[-1])])
            gates,held=observation(r)
            g=next(g for g in gates if (g['onset_index'],g['voice'])==(1,peer))
            self.assertGreater(g['gate_spc_cycles'],240000)
            self.assertTrue(all(e['retrigger_half_cycle']==0 for e in r['envelopes']))

    def test_actual_DSP_release_envelope_PCM_reset_and_restore(self):
        for r in self.reports.values():
            self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertEqual(r['final_peer_stop'],1);self.assertEqual(r['final_mode'],0)
            self.assertGreaterEqual(r['pcm']['quiet_tail_frames'],64)
            self.assertGreater(r['pcm']['nonzero_frames'],0)
            for env in r['envelopes']:
                self.assertGreater(env['off_half_cycle'],env['on_half_cycle'])
                self.assertGreater(env['release_steps'],0);self.assertTrue(env['release_zero'])

    def test_prior_completion_cannot_qualify_new_pulse(self):
        for c in ('clip-2','clip-3'):
            with self.assertRaises(ValueError):
                gate_native(PROBE,bank(127,c),builder=prior_build,validator=validate,output_bound=131072)
        for c in ('inactive-2','inactive-3'):
            with self.assertRaisesRegex(ValueError,'sparse inactive'):
                gate_native(PROBE,bank(127,c),builder=prior_build,validator=lambda r:None,output_bound=131072)

    def test_guard_retains_preceding_completion_for_other_geometry(self):
        for data in (sparse_bank(),sparse_bank(case='transitions')):
            r=gate_native(PROBE,data,builder=build,validator=lambda r:None,output_bound=131072)
            self.assertEqual(r['status'],2);self.assertEqual(r['final_peer_stop'],0)
            self.assertNotIn(255,[w['value'] for w in r['key_writes']])
        with self.assertRaises(ValueError):bank(True)
        with self.assertRaises(ValueError):bank(case='unknown')

    def test_removed_final_KOF_latch_hold_and_inactive_freeze_are_detected(self):
        for before,after in (('    mov $f3, #$ff\n    ; Reuse','    mov $f3, #$00\n    ; Reuse'),
                             ('    call duet_wait\n    mov $f3, #$00\n    mov $f2, #$4c\n','    mov $f3, #$00\n    mov $f2, #$4c\n'),
                             ('reselect_pulse3:\n    mov a, $a3\n    and a, #$08\n    beq gates_expire\n','reselect_pulse3:\n')):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):
                gate_native(PROBE,bank(127,'inactive-2'),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_false_native_write_release_envelope_and_completion_metadata(self):
        original=self.reports['inactive-2-127']
        for mutation in ('pulse','held','source','envelope','pcm','restore','phase','edge','type'):
            r=copy.deepcopy(original)
            if mutation=='pulse':r['key_writes'][-3]['value']=12
            elif mutation=='held':r['keyoffs'][-1]['held_mask']=12
            elif mutation=='source':r['keyoffs'][-1]['cause']=1
            elif mutation=='envelope':r['envelopes'][3]['release_zero']=False
            elif mutation=='pcm':r['pcm']['quiet_tail_frames']=0
            elif mutation=='restore':r['restore_equal']=False
            elif mutation=='phase':r['final_peer_stop']=0
            elif mutation=='edge':r['keyoffs'].pop()
            elif mutation=='type':r['key_writes'][-3]['value']=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)

    def test_bounded_synthetic_reference_matrix_and_malformed_evidence(self):
        refs=references();self.assertEqual(len(agree(refs)),8)
        for mutation in ('held','source','pulse','retrigger','end','types','post-write','window'):
            r=copy.deepcopy(next(r for r in refs if r['case']=='inactive-2' and r['articulation']==127))
            if mutation=='held':r['voices_pending_at_stop']=[]
            elif mutation=='source':r['note_gates'][3]['release_kind']='timer-keyoff'
            elif mutation=='pulse':r['final_control_writes'][0]['value']=12
            elif mutation=='retrigger':r['unreleased_retriggers']=[3]
            elif mutation=='end':r['unreleased_final_voices']=[3]
            elif mutation=='types':r['articulation']=True
            elif mutation=='post-write':r['post_stop_nonzero_control_writes']=[{'address':76,'value':8}]
            elif mutation=='window':r['post_stop_observation_spc_cycles']=1
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):contract(r,r['case'])
        with self.assertRaises(ValueError):agree(refs[:-1])
        with self.assertRaises(ValueError):agree(refs[:-1]+[refs[0]])
        bad=copy.deepcopy(refs);bad[1]['onset_intervals_spc_cycles'][0]+=3000
        with self.assertRaises(ValueError):agree(bad)
        for text in ('bad header\n','x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'clip-2')
        rows='kind,master_clock,spc_cycle,pcm_sample,address,value\n'+'D,0,0,0,61,0\n'*32768
        with self.assertRaisesRegex(ValueError,'row bound'):observe(io.StringIO(rows),'clip-2')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
