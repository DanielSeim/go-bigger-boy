#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ready boundary notes through immediate rests: crossed profiles and real edges."""
import argparse,copy,hashlib,io,json
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_follow_rest import build,source
from build_sgb_score_pending import build as prior_build
from build_sgb_score_follow_rest_fixture import bank,build as fixture_build,CASES,DURATIONS
from check_sgb_score_follow_rest_reference import observe,contract,agree,expected,ticks
from check_sgb_score_follow_rest_playback import native,align,validate,identity,volume_updates
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_peer_reference import native_observation
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
from sgb_score_sparse_tests import authored
PROBE=None


def trace(case,a,d,s,b):
    events=[]
    for i,(edge,tick) in enumerate(zip(expected(case),ticks(s))):
        cycle=10000+tick*5500
        for v in edge['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            events.extend((cycle,16*v['voice']+r,value) for r,value in enumerate(values))
            gate=PULSES[(96,b if i in (2,3) else a,s if i==2 else 16)]*2048
            events.append((cycle+gate,0x5C,1<<v['voice']))
        events.append((cycle,0x4C,edge['mask']))
    events.sort(key=lambda e:e[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\nD,0,0,0,61,0\n'+''.join(f'D,0,{c},0,{a},{v}\n' for c,a,v in events)


def references():
    result=[]
    for c in CASES:
        for d in DURATIONS:
            for s in DURATIONS:
                for a in (63,127):
                    for b in (63,127):
                        r=observe(io.StringIO(trace(c,a,d,s,b)),c,a,d,s,b)
                        result.extend({**copy.deepcopy(r),'case':c,'model':m} for m in ('sgb','sgb2'))
    return result


class FollowRestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{d}-{s}-{a}-{b}':native(PROBE,bank(a,d,s,c,b)) for c in CASES for d in DURATIONS for s in DURATIONS for a in (63,127) for b in (63,127)}

    def test_reproducible_owned_inputs_and_prior_image(self):
        self.assertEqual(build(),build());self.assertLessEqual(len(build()),4096)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'5914bedd6cb346464b442d8ec6f0af3df5e19c6f41d957bcf0d35331f2162d53')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'c55ef2d6f97b469cf42bd56efbe1e519da9088350bd4b1ad17111ee9b242e117')
        hashes=set()
        for r in self.reports.values():
            c,a,d,s,b=identity(r);data=bank(a,d,s,c,b);align(r,data)
            self.assertEqual(len(data),2048)
            self.assertEqual(r['pattern_masks'],[12,12,4,12])
            self.assertEqual(r['pattern_ticks'],[0,32,48+s,80+s])
            hashes.add(hashlib.sha256(fixture_build(a,d,s,c,b)).hexdigest())
        self.assertEqual(len(hashes),32)

    def test_pending_pitch_old_volume_and_raw_rest_without_DSP_writes(self):
        for r in self.reports.values():
            c,a,d,s,b=identity(r);es=r['events'];edge=r['keyons'][2]
            self.assertEqual(r['event_patterns'][4:7],[0,1,1])
            self.assertEqual([(e['tick'],e['channel']) for e in es[4:7]],[(32,2),(32,2),(32,3)])
            self.assertEqual(es[5]['opcode'],0xC9);self.assertEqual(es[5]['volumes'],[0,0])
            writes=[w for w in r['voice_writes'] if es[5]['half_cycle']<=w['half_cycle']<es[6]['half_cycle']]
            self.assertEqual(writes,[])
            self.assertEqual(edge['mask'],12 if c=='boundary-note' else 8)
            if c=='boundary-note':
                self.assertEqual(edge['pitches'][0],1700);self.assertEqual(edge['volumes'][0],[7,7])
                self.assertEqual(es[4]['volumes'],[1,1])
                self.assertEqual(edge['pending_pulses'][0],PULSES[(96,b,s)])
            self.assertEqual(volume_updates(r)[0]['volumes'],[1,1])
            self.assertEqual(r['keyons'][3]['volumes'][0],[1,1])

    def test_crossed_profiles_release_before_next_note(self):
        for b in (63,127):
            for s in DURATIONS:
                gates=[]
                for a in (63,127):
                    for d in DURATIONS:
                        r=self.reports[f'boundary-note-{d}-{s}-{a}-{b}'];gs,rs=native_observation(r)
                        self.assertEqual(rs,[]);self.assertEqual(len(gs),14)
                        g=next(g for g in gs if (g['onset_index'],g['voice'])==(2,2));gates.append(g['gate_spc_cycles'])
                        self.assertEqual(g['cause'],1)
                        self.assertLess(g['gate_spc_cycles'],(r['keyons'][3]['half_cycle']-r['keyons'][2]['half_cycle'])/2)
                        env=next(e for e in r['envelopes'] if e['voice']==2 and e['on_half_cycle']==r['keyons'][2]['half_cycle'])
                        self.assertEqual(env['retrigger_half_cycle'],0);self.assertGreater(env['release_steps'],0)
                self.assertLessEqual(max(gates)-min(gates),2048)

    def test_prior_image_and_conservative_guards_reject_silently(self):
        r=gate_native(PROBE,bank(),builder=prior_build,validator=validate,output_bound=131072)
        self.assertEqual(r['status'],226)
        base=bank();bad=bytearray(base);bad[0x6BD]=24
        prefix=bytearray(base);prefix[0x6BD:0x6BD+9]=bytes((0xE0,2,8,127,0xC9,16,127,0x9C,0))
        boundary=bytearray(base);boundary[0x2F1+16]=24
        solo=bytearray(base);solo[0x3FB+6:0x3FB+8]=bytes(2)
        pending=(16,127,0x98,0x99,8,127,0xA0,0);end=(16,127,0x98,0x99,0)
        for data,tempo in ((bad,96),(prefix,96),(boundary,96),(solo,96),(base,128),(authored([(pending,end)]),96)):
            r=native(PROBE,bytes(data),tempo);self.assertEqual(r['status'],226);self.assertEqual(r['voice_writes'],[])

    def test_missing_pending_rest_gate_fault_detected(self):
        # A zero cached rest profile must not overwrite the surviving note's
        # measured profile at the actual KON. Use crossed duration/articulation.
        broken=source().replace('    mov a, $b9\n    mov $78, a\n','    mov $78, #$00\n',1)
        with self.assertRaises(ValueError):gate_native(PROBE,bank(63,16,8,'boundary-note',127),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_false_raw_physical_and_envelope_metadata_reject(self):
        r=self.reports['boundary-note-16-8-63-127']
        for m in ('mask','volume','rest','pattern','pulse','write','release','time','type','pcm'):
            bad=copy.deepcopy(r)
            if m=='mask':bad['keyons'][2]['mask']=8
            elif m=='volume':bad['keyons'][2]['volumes'][0]=[1,1]
            elif m=='rest':bad['events'][5]['articulation']=63
            elif m=='pattern':bad['event_patterns'][4]=1
            elif m=='pulse':bad['keyons'][2]['pending_pulses'][0]=23
            elif m=='write':bad['voice_writes'].pop()
            elif m=='release':bad['envelopes'][4]['retrigger_half_cycle']=bad['keyons'][3]['half_cycle']
            elif m=='time':bad['keyons'][2]['half_cycle']=bad['events'][4]['half_cycle']+1
            elif m=='type':bad['events'][5]['duration']=True
            else:bad['pcm']['quiet_tail_frames']=0
            with self.assertRaises(ValueError):validate(bad)

    def test_original_contract_and_complete_intermodel_guards(self):
        refs=references();self.assertEqual(len(agree(refs)),32)
        for m in ('duplicate','gate','type','onset','early-volume','mask','pitch'):
            bad=copy.deepcopy(refs)
            if m=='duplicate':bad[-1]=bad[0]
            elif m=='gate':bad[1]['note_gates'][0]['gate_spc_cycles']+=3000
            elif m=='type':bad[0]['rest_articulation']=True
            elif m=='onset':bad[1]['onset_intervals_spc_cycles'][0]+=3000
            elif m=='early-volume':bad[0]['voice2_volume_updates'][0]['interval_spc_cycles']=0
            elif m=='mask':bad[0]['keyons'][2]['mask']=8
            else:bad[0]['boundary_pitch_writes'].reverse()
            with self.assertRaises(ValueError):agree(bad)
        bad=copy.deepcopy(refs)
        for r in bad:
            if (r['case'],r['articulation'],r['rest_articulation'],r['rest_duration'])==('boundary-note',127,127,8):
                next(g for g in r['note_gates'] if (g['onset_index'],g['voice'])==(2,2))['gate_spc_cycles']+=2100
        with self.assertRaises(ValueError):agree(bad)
        for text in ('bad\n','x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'boundary-note')
        for args in ((True,8,8,'boundary-note',127),(127,24,8,'boundary-note',127),(127,8,8,'boundary-note',True)):
            with self.assertRaises(ValueError):bank(*args)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
