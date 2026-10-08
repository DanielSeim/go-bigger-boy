#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen inactive gates, unreleased retriggers and resumed rest countdowns."""
import argparse,copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_peer import build,source
from build_sgb_score_tail import build as prior_build
from build_sgb_score_peer_fixture import bank,expected,ticks,CASES
from build_sgb_prototype import assemble
from check_sgb_score_peer_reference import native,align,validate,observe,contract,compare,native_observation
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_gate_reference import PULSES
from sgb_score_sparse_tests import authored
PROBE=None


def trace(case,art):
    rows=['kind,master_clock,spc_cycle,pcm_sample,address,value\n','D,0,0,0,61,0\n']
    for i,(edge,tick) in enumerate(zip(expected(case),ticks(case))):
        cycle=1000+tick*5500
        for v in edge['voices']:
            vals=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,0x8F,0x6F,0xB8)
            rows.extend(f'D,0,{cycle},0,{v["voice"]*16+r},{value}\n' for r,value in enumerate(vals))
        off=edge['mask']
        if case.startswith('clip') and art==127 and i==1:off=1<<int(case[-1])
        rows.extend((f'D,0,{cycle},0,76,{edge["mask"]}\n',f'D,0,{cycle+70000},0,92,{off}\n'))
    return ''.join(rows)


class PeerTests(unittest.TestCase):
    def render(self,patterns,tempo=96):
        data=authored(patterns);report=native(PROBE,data,tempo);align(report,data)
        self.assertEqual(report['status'],2)
        self.assertTrue(report['source_unmodified']);self.assertTrue(report['cache_guards_equal'])
        return report

    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4016)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'f0616a5014fdf2f3dc4d6b1ccafcf5192501f4aec0d436cc513a6fa00000c182')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'f5455964ec1246b4cf6798d0d38c812cdd26dad87924029a3341ac1b3e70caf2')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_owned_clipping_and_rest_cases(self):
        for c in CASES:
            for a in (63,127):
                data=bank(a,c);r=native(PROBE,data);align(r,data)
                gates,retriggers=native_observation(r)
                missing=c.startswith('clip') and a==127
                self.assertEqual(len(gates),11 if missing else 12)
                self.assertEqual(len(retriggers),1 if missing else 0)
                self.assertTrue(all(g['cause']==1 for g in gates))
                if c[-1]=='2' and a==127:self.assertGreater(r['frozen_peer_checks'][1],100000)
                for e,w in zip(r['keyons'],expected(c)):
                    for v in w['voices']:self.assertEqual(e['volumes'][v['voice']-2],v['volumes'])

    def test_unreleased_envelope_has_retrigger_not_fake_off(self):
        r=native(PROBE,bank(127,'clip-2'))
        env=r['envelopes'][3]
        self.assertEqual(env['voice'],3);self.assertEqual(env['off_half_cycle'],0)
        self.assertEqual(env['retrigger_half_cycle'],r['keyons'][4]['half_cycle'])
        self.assertEqual(env['release_steps'],0);self.assertFalse(env['release_zero'])
        self.assertEqual(env['peak'],127);self.assertGreater(env['decay_min'],0)

    def test_frozen_counter_survives_two_inactive_patterns(self):
        short=(8,127,0x98,0);long=(24,127,0x99,0);pair=(16,127,0x9A,0x9B,0)
        r=self.render([(short,long),(pair,None),(pair,None),(None,(16,127,0xA4,0))])
        self.assertGreater(r['frozen_peer_checks'][1],500000)
        gates,rs=native_observation(r)
        self.assertEqual(len(rs),1);self.assertEqual(rs[0]['voice'],3)
        self.assertEqual(rs[0]['previous_onset_index'],0)
        self.assertTrue(all(g['cause']==1 for g in gates))

    def test_rest_resumes_remaining_counter_without_immediate_kof(self):
        r=native(PROBE,bank(127,'rest-2'));align(r,bank(127,'rest-2'))
        gates,rs=native_observation(r);self.assertEqual(rs,[])
        peer=next(g for g in gates if g['voice']==3 and g['onset_index']==1)
        self.assertGreater(peer['gate_spc_cycles'],250000)
        rest=next(e for e in r['events'] if e['channel']==3 and e['opcode']==0xC9)
        edge=next(e for e in r['keyoffs'] if e['half_cycle']==r['keyons'][1]['half_cycle']+int(2*peer['gate_spc_cycles']))
        self.assertGreater(edge['half_cycle'],rest['half_cycle']+10000)
        self.assertEqual(edge['cause'],1)

    def test_short_gate_still_releases_before_clipping(self):
        for c in ('clip-2','clip-3'):
            r=native(PROBE,bank(63,c));gates,rs=native_observation(r)
            self.assertEqual(len(gates),12);self.assertEqual(rs,[])
            self.assertEqual(r['frozen_peer_checks'],[0,0])

    def test_all_profiles_and_full_cache_preserve_normal_releases(self):
        for tempo,art,duration in PULSES:
            note=(0xE0,2,duration,art,*([0x98]*8),0)
            later=(*([0x99]*8),0)
            r=self.render([(note,note),*([(later,later)]*3)],tempo)
            self.assertEqual(len(r['events']),64)
            gates,rs=native_observation(r);self.assertEqual(len(gates),64);self.assertEqual(rs,[])
            self.assertEqual(r['frozen_peer_checks'],[0,0])

    def test_missing_freeze_is_detected_by_physical_observer(self):
        text=source().replace('    mov a, $a3\n    and a, #$08\n    beq gates_expire\n','')
        image=bytes(assemble(text,'spc',0x0800))
        with self.assertRaises(ValueError):gate_native(PROBE,bank(127,'clip-2'),builder=lambda:image,validator=validate,output_bound=131072)

    def test_old_forced_release_does_not_prove_freeze(self):
        r=gate_native(PROBE,bank(127,'clip-2'),builder=prior_build,validator=validate,output_bound=131072)
        self.assertEqual(r['frozen_peer_checks'],[0,0])
        self.assertEqual(len(native_observation(r)[1]),0)
        self.assertTrue(any(g['cause']==0 for g in native_observation(r)[0]))

    def test_false_envelope_and_counter_metadata_reject(self):
        r=native(PROBE,bank(127,'clip-2'))
        for mutation in ('retrigger','off','count','type'):
            bad=copy.deepcopy(r)
            if mutation=='retrigger':bad['envelopes'][3]['retrigger_half_cycle']=0
            elif mutation=='off':bad['envelopes'][3]['off_half_cycle']=bad['envelopes'][3]['retrigger_half_cycle']
            elif mutation=='count':bad['frozen_peer_checks'][1]=-1
            else:bad['frozen_peer_checks'][1]=True
            with self.assertRaises(ValueError):validate(bad)

    def test_reference_matrix_and_missing_release_mutations(self):
        candidates={};refs=[]
        for c in CASES:
            for a in (63,127):
                r=native(PROBE,bank(a,c));candidates[f'{c}-{a}']=r
                result=observe(io.StringIO(trace(c,a)),c)
                gates,rs=native_observation(r)
                result['note_gates']=[{'voice':g['voice'],'onset_index':g['onset_index'],'gate_spc_cycles':int(g['gate_spc_cycles']),'release_kind':'keyoff'} for g in gates]
                result['unreleased_retriggers']=[{**v,'interval_spc_cycles':int(v['interval_spc_cycles'])} for v in rs]
                result['onset_intervals_spc_cycles']=[int((b['half_cycle']-a['half_cycle'])/2) for a,b in zip(r['keyons'],r['keyons'][1:])]
                for model in ('sgb','sgb2'):refs.append({**copy.deepcopy(result),'case':c,'articulation':a,'model':model})
        self.assertEqual(len(compare(candidates,refs)),16)
        for mutation in ('retrigger','gate','duplicate','type'):
            bad=copy.deepcopy(refs);index=next(i for i,v in enumerate(bad) if v['case']=='clip-2' and v['articulation']==127)
            if mutation=='retrigger':bad[index]['unreleased_retriggers']=[]
            elif mutation=='gate':bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
            elif mutation=='duplicate':bad[-1]=bad[0]
            else:bad[0]['note_gates'][0]['voice']=True
            with self.assertRaises(ValueError):compare(candidates,bad)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True)
    args,remaining=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
