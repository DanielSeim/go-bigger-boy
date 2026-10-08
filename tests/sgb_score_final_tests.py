#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Final-boundary raw execution, real stop pulse and held versus quiet completion."""
import argparse,copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_final import build,source
from build_sgb_score_follow_rest import build as prior_build
from build_sgb_score_final_fixture import bank,build as fixture_build,CASES,DURATIONS
from check_sgb_score_final_reference import expected,observe,contract,agree
from check_sgb_score_final_playback import native,validate,identity,align,control_writes,observation,SCHEMA
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
PROBE=None


def trace(case,a,d):
    events=[(0,61,0)]
    for i,e in enumerate(expected(case)[:2]):
        cycle=10000+i*88000
        for v in e['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            events.extend((cycle,16*v['voice']+r,value) for r,value in enumerate(values))
            events.append((cycle+PULSES[(96,a,16)]*2048,92,1<<v['voice']))
        events.append((cycle,76,12))
    if case=='final-note':events.extend(((185600,34,164),(185636,35,6)))
    events.extend(((186000,92,255),(186117,92,0),(186156,76,4 if case=='final-note' else 0),(186500,76,0),(500000,61,0)))
    events.sort(key=lambda e:e[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+''.join(f'D,0,{c},0,{r},{v}\n' for c,r,v in events)


def references():
    result=[]
    for c in CASES:
        for d in DURATIONS:
            for a in (63,127):
                r=observe(io.StringIO(trace(c,a,d)),c,a,d)
                result.extend({**copy.deepcopy(r),'case':c,'model':m} for m in ('sgb','sgb2'))
    return result


class FinalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{d}-{a}':native(PROBE,bank(a,d,c)) for c in CASES for d in DURATIONS for a in (63,127)}

    def test_reproducible_owned_corpus_and_prior_image(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4059)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'00ff6c59ecdb5915b9cb2e742b45b17bb1422438150923c0be2cc6877d7ddf7b')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'5914bedd6cb346464b442d8ec6f0af3df5e19c6f41d957bcf0d35331f2162d53')
        hashes=set()
        for r in self.reports.values():
            c,a,d=identity(r);data=bank(a,d,c);align(r,data)
            self.assertEqual(len(data),2048);self.assertEqual(data[0x101:0x103],bytes(2))
            hashes.add(hashlib.sha256(fixture_build(a,d,c)).hexdigest())
        self.assertEqual(len(hashes),8)

    def test_last_raw_record_at_end_tick_without_volume_application(self):
        for r in self.reports.values():
            c,a,d=identity(r);e=r['events'][-1]
            self.assertEqual((e['tick'],r['end_tick'],e['channel']),(32,32,2))
            self.assertEqual(r['event_patterns'],[0]*5);self.assertEqual(e['track_volume'],64)
            ws=[w for w in r['voice_writes'] if w['half_cycle']>=e['half_cycle']]
            self.assertEqual([w['address'] for w in ws],[0x22,0x23] if c=='final-note' else [])
            self.assertEqual(e['volumes'],[1,1] if c=='final-note' else [0,0])
            with self.assertRaises(ValueError):multi_validate(r,schema=SCHEMA,max_events=64,max_ticks=2032,min_events=1,initial_pair=False)

    def test_actual_final_stop_pulse_and_pending_KON(self):
        for r in self.reports.values():
            c,a,d=identity(r);ws=control_writes(r)
            self.assertEqual([(w['address'],w['value']) for w in ws],[(92,255),(92,0),(76,4 if c=='final-note' else 0)])
            gates,held=observation(r)
            self.assertEqual(len(gates),4);self.assertTrue(all(g['cause']==1 for g in gates))
            self.assertEqual(held,[2] if c=='final-note' else [])
            if c=='final-note':
                self.assertEqual(r['keyons'][-1]['pitches'][0],1700)
                self.assertEqual(r['keyons'][-1]['volumes'][0],[7,7])
                self.assertEqual(r['keyons'][-1]['pending_pulses'],[0,0])

    def test_held_tail_restore_and_rest_silence_are_distinct(self):
        for r in self.reports.values():
            c,a,d=identity(r)
            self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertEqual(r['final_observation_half_cycles'],600000)
            if c=='final-note':
                self.assertGreater(r['final_env_start'],0);self.assertGreater(r['final_env_end'],0)
                env=r['envelopes'][-1]
                self.assertEqual(env['off_half_cycle'],0);self.assertEqual(env['retrigger_half_cycle'],0)
                self.assertEqual(env['release_steps'],0);self.assertFalse(env['release_zero'])
            else:self.assertEqual(r['final_env_end'],0);self.assertGreaterEqual(r['pcm']['quiet_tail_frames'],64)

    def test_conservative_prior_and_unknown_guards(self):
        for c in CASES:
            r=gate_native(PROBE,bank(case=c),builder=prior_build,validator=validate,output_bound=131072)
            self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[])
        base=bank();duration=bytearray(base);duration[0x2F1+16]=24
        preceding=bytearray(base);preceding[0x2F1+10:0x2F1+21]=bytes((24,127,0x98,8,127,0x99,0xED,64,8,127,0xA0));preceding[0x2F1+21]=0
        prefix=bytearray(base);prefix[0x2F1+14:0x2F1+23]=bytes((0xE0,2,0xED,64,8,127,0xA0,0,0))
        for data,tempo in ((duration,96),(preceding,96),(prefix,96),(base,128)):
            r=native(PROBE,bytes(data),tempo);self.assertEqual(r['status'],226);self.assertEqual(r['voice_writes'],[])

    def test_discarded_final_KON_and_forced_release_faults(self):
        for before,after in (('    mov a, $51\n    mov $f3, a\n    beq final_clear\n','    mov a, #$00\n    mov $f3, a\n    beq final_clear\n'),('    call final_complete\n','    call poly_complete\n')):
            broken=source().replace(before,after,1)
            with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_false_terminal_raw_edge_envelope_and_PCM_metadata(self):
        r=self.reports['final-note-8-127']
        for m in ('tick','volume','mode','window','release','zero','kon','key-write','cause','type'):
            bad=copy.deepcopy(r)
            if m=='tick':bad['events'][-1]['tick']=31
            elif m=='volume':bad['keyons'][-1]['volumes'][0]=[1,1]
            elif m=='mode':bad['final_mode']=0
            elif m=='window':bad['final_observation_half_cycles']=40000
            elif m=='release':bad['envelopes'][-1]['off_half_cycle']=bad['completion_half_cycle']
            elif m=='zero':bad['final_env_end']=0
            elif m=='kon':bad['keyons'].pop()
            elif m=='key-write':control_writes(bad)[0]['value']=12
            elif m=='cause':bad['keyoffs'][0]['cause']=0
            else:bad['events'][-1]['duration']=True
            with self.assertRaises(ValueError):validate(bad)

    def test_original_observer_and_complete_matrix_guards(self):
        refs=references();self.assertEqual(len(agree(refs)),8)
        for m in ('duplicate','gate','held','short','control','type','volume','interval'):
            bad=copy.deepcopy(refs)
            if m=='duplicate':bad[-1]=bad[0]
            elif m=='gate':bad[1]['note_gates'][0]['gate_spc_cycles']+=3000
            elif m=='held':bad[0]['unreleased_final_voices']=[]
            elif m=='short':bad[0]['held_observation_spc_cycles']=20000
            elif m=='control':bad[0]['final_control_writes'][0]['value']=12
            elif m=='type':bad[0]['boundary_duration']=True
            elif m=='volume':bad[0]['post_second_onset_volume_writes']=[{'address':32,'value':1}]
            else:bad[1]['final_stop_interval_spc_cycles']+=3000
            with self.assertRaises(ValueError):agree(bad)
        for text in ('bad\n','x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'final-note')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
