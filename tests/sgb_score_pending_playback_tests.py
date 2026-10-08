#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical pending KON, delayed volume, rest release and conservative guards."""
import argparse,copy,hashlib
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_pending import build,source
from build_sgb_score_reverse import build as prior_build
from build_sgb_score_pending_fixture import bank,CASES,DURATIONS
from check_sgb_score_pending_playback import native,align,validate,volume_updates,SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_peer_reference import native_observation
from check_sgb_score_reverse_reference import align as reverse_align,SCHEMA as REVERSE_SCHEMA
from build_sgb_score_reverse_fixture import bank as reverse_bank,CASES as REVERSE_CASES
from build_sgb_prototype import assemble
from sgb_score_sparse_tests import authored
PROBE=None


class PendingPlaybackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{d}-{a}':native(PROBE,bank(a,d,c)) for c in CASES for d in DURATIONS for a in (63,127)}

    def test_reproducible_prior_image(self):
        self.assertEqual(build(),build());self.assertLessEqual(len(build()),4096)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'c55ef2d6f97b469cf42bd56efbe1e519da9088350bd4b1ad17111ee9b242e117')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'deece9488c7138fb4e9f0cde22beccf2edd3770f42b1df2a257f9b387cd15dda')

    def test_raw_logical_controls_and_actual_pending_writes(self):
        for key,r in self.reports.items():
            c,a,d=key.rsplit('-',2);align(r,bank(int(d),int(a),c))
            e=r['events'][4];next_e=r['events'][5]
            self.assertEqual(e['tick'],32);self.assertEqual(r['event_patterns'][4],0)
            self.assertEqual(e['track_volume'],64)
            writes=[w['address'] for w in r['voice_writes'] if e['half_cycle']<=w['half_cycle']<next_e['half_cycle']]
            self.assertEqual(writes,[] if c=='pending-rest' else [0x22,0x23])
            self.assertEqual(r['keyons'][2]['mask'],8 if c=='pending-rest' else 12)
            self.assertEqual(volume_updates(r)[0]['volumes'],[1,1])
            if c!='pending-rest':
                self.assertEqual(r['keyons'][2]['volumes'][0],[7,7]);self.assertGreater(r['frozen_peer_checks'][0],0)
                self.assertEqual(e['volumes'],[1,1])

    def test_return_note_has_retrigger_without_release(self):
        for d in DURATIONS:
            for a in (63,127):
                r=self.reports[f'note-return-{d}-{a}'];g,rs=native_observation(r)
                self.assertEqual(len(g),12);self.assertEqual(len(rs),1)
                self.assertEqual((rs[0]['voice'],rs[0]['previous_onset_index'],rs[0]['onset_index']),(2,2,4))
                env=next(e for e in r['envelopes'] if e['voice']==2 and e['on_half_cycle']==r['keyons'][2]['half_cycle'])
                self.assertEqual(env['off_half_cycle'],0);self.assertEqual(env['release_steps'],0)
                self.assertEqual(env['retrigger_half_cycle'],r['keyons'][4]['half_cycle'])

    def test_return_rest_rearms_from_rest_not_pending_duration(self):
        for c in ('rest-return','long-rest-return'):
            for a in (63,127):
                gates=[]
                for d in DURATIONS:
                    r=self.reports[f'{c}-{d}-{a}'];g,rs=native_observation(r)
                    gates.append(next(x['gate_spc_cycles'] for x in g if (x['onset_index'],x['voice'])==(2,2)))
                    self.assertEqual(rs,[]);self.assertEqual(len(g),13)
                    rest=next(e for e in r['events'] if e['tick']==64 and e['channel']==2)
                    following=next(e for e in r['events'] if e['channel']==2 and e['tick']>64)
                    self.assertEqual(rest['opcode'],0xC9)
                    self.assertFalse(any(rest['half_cycle']<=w['half_cycle']<following['half_cycle'] for w in r['voice_writes']))
                self.assertLessEqual(abs(gates[0]-gates[1]),2048)
        for a in (63,127):
            short=native_observation(self.reports[f'rest-return-8-{a}'])[0]
            long=native_observation(self.reports[f'long-rest-return-8-{a}'])[0]
            pick=lambda gs:next(g['gate_spc_cycles'] for g in gs if (g['onset_index'],g['voice'])==(2,2))
            self.assertGreater(pick(long)-pick(short),25000)

    def test_previous_reverse_corpus(self):
        for c in REVERSE_CASES:
            for a in (63,127):
                data=reverse_bank(a,c)
                gate_native(PROBE,data,builder=build,validator=lambda r:reverse_align({**r,'schema':REVERSE_SCHEMA},data),output_bound=131072)

    def test_unmeasured_rest_prefix_duration_tempo_and_final_reject(self):
        data=bank(127,8,'rest-return')
        bad=bytearray(data);bad[0x6DD]=24
        prefix=bytearray(data);prefix[0x6DD:0x6DD+10]=bytes((0xE0,2,8,127,0xC9,16,127,0x9C,0x9D,0))
        boundary=bytearray(bank());boundary[0x2F1+16]=24
        # Unknown final stop / no subsequent reactivation remain excluded.
        pending=(16,127,0x98,0x99,8,127,0xA0,0);end=(16,127,0x98,0x99,0)
        for b,t in ((bad,96),(prefix,96),(boundary,96),(bank(),128),(authored([(pending,end)]),96),(authored([(pending,end),(None,end)]),96)):
            r=native(PROBE,bytes(b),t);self.assertEqual(r['status'],226);self.assertEqual(r['voice_writes'],[])

    def test_false_physical_logical_and_envelope_metadata_reject(self):
        r=self.reports['note-return-8-127']
        for m in ('vol','write','logical','pattern','frozen','release','pulse','time','type','pcm'):
            bad=copy.deepcopy(r)
            if m=='vol':bad['keyons'][2]['volumes'][0]=[1,1]
            elif m=='write':bad['voice_writes'].insert(8,{**bad['voice_writes'][8],'address':0x20,'value':1})
            elif m=='logical':bad['events'][4]['track_volume']=127
            elif m=='pattern':bad['event_patterns'][4]=1
            elif m=='frozen':bad['frozen_peer_checks'][0]=0
            elif m=='release':next(e for e in bad['envelopes'] if e['retrigger_half_cycle'])['off_half_cycle']=100
            elif m=='pulse':bad['keyons'][2]['pending_pulses'][0]=36
            elif m=='time':bad['keyons'][2]['half_cycle']=bad['events'][4]['half_cycle']+1
            elif m=='type':bad['events'][4]['duration']=True
            else:bad['pcm']['quiet_tail_frames']=0
            with self.assertRaises(ValueError):validate(bad)

    def test_rest_retained_counter_fault_detected(self):
        broken=source().replace('    mov $72, a\n    ret\npending_bad:', '    ret\npending_bad:',1)
        with self.assertRaises(ValueError):gate_native(PROBE,bank(127,8,'long-rest-return'),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_missing_pending_KON_fault_detected(self):
        broken=source().replace('    mov a, $b6\n    mov $51, a\n','    mov $51, #$00\n',1)
        with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
