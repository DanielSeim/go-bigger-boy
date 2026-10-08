#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Priority skips, inherited controls and conservative reverse-order rejection."""
import argparse,copy,hashlib,io,json,subprocess,tempfile
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_order import build
from build_sgb_score_peer import build as prior_build
from build_sgb_score_order_fixture import bank,expected,CASES,QUALIFIED_CASES
from build_sgb_score_peer_fixture import bank as peer_bank,CASES as PEER_CASES
from check_sgb_score_order_reference import native,align,validate,observe,contract,compare
from check_sgb_score_peer_reference import native_observation
from sgb_score_sparse_tests import authored
PROBE=None


def trace(case):
    rows=['kind,master_clock,spc_cycle,pcm_sample,address,value\n','D,0,0,0,61,0\n']
    for i,edge in enumerate(expected(case)):
        cycle=1000+i*88000
        if i==2 and case=='end-3-note':
            rows.extend((f'D,0,{cycle},0,34,164\n',f'D,0,{cycle},0,35,6\n'))
        for v in edge['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            rows.extend(f'D,0,{cycle},0,{16*v["voice"]+r},{value}\n' for r,value in enumerate(values))
        rows.extend((f'D,0,{cycle},0,76,{edge["mask"]}\n',f'D,0,{cycle+70000},0,92,{edge["mask"]}\n'))
    return ''.join(rows)


class OrderTests(unittest.TestCase):
    def test_reproducible_prior_unchanged(self):
        self.assertEqual(len(build()),4016)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'2efc1a89303d11f373991727492284819dd0d2fdff8ab750d8b770c12773a56a')
        self.assertEqual(build(),build())
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'f0616a5014fdf2f3dc4d6b1ccafcf5192501f4aec0d436cc513a6fa00000c182')

    def test_skipped_note_rest_controls_and_timing(self):
        for case in QUALIFIED_CASES:
            for art in (63,127):
                data=bank(art,case);r=native(PROBE,data);align(r,data)
                self.assertEqual(r['pattern_ticks'],[0,32,64,96]);self.assertEqual(r['end_tick'],128)
                self.assertEqual(len(r['events']),12);self.assertEqual(len(r['keyons']),8)
                self.assertEqual(r['frozen_peer_checks'],[0,0])
                # The skipped ED64 and duration8/art63 must not change the
                # returning voice's inherited fields or create a phantom event.
                returning=[e for e in r['events'] if e['channel']==3 and e['tick']>=64]
                self.assertTrue(all(e['track_volume']==127 and e['articulation']==art and e['duration']==16 for e in returning))
                gates,rs=native_observation(r);self.assertEqual(len(gates),12);self.assertEqual(rs,[])
                self.assertTrue(all(g['cause']==1 for g in gates))

    def test_skipped_timing_does_not_seed_implicit_return(self):
        for opcode in (0xA0,0xC9):
            for art in (63,127):
                ending=(16,art,0x98,0x99,0)
                peer=(16,art,0x98,0x99,0xED,64,8,63,opcode,0)
                data=authored([(ending,peer),((0x9A,0x9B,0),None),(None,(0x9C,0x9D,0))])
                r=native(PROBE,data);align(r,data)
                returning=[e for e in r['events'] if e['channel']==3 and e['tick']>=64]
                self.assertEqual(len(returning),2)
                self.assertTrue(all(e['duration']==16 and e['articulation']==art and e['track_volume']==127 for e in returning))
                self.assertEqual(r['end_tick'],96)

    def test_preceding_peer_cases_still_align(self):
        for case in PEER_CASES:
            data=peer_bank(127,case);r=native(PROBE,data);align(r,data)

    def test_prior_image_and_reverse_order_reject_silently(self):
        with tempfile.TemporaryDirectory() as directory:
            p,d=Path(directory)/'program',Path(directory)/'bank'
            for image,case in [(prior_build(),'end-2-note'),*[(build(),c) for c in CASES[2:]]]:
                p.write_bytes(image);d.write_bytes(bank(127,case))
                result=subprocess.run([str(PROBE),str(p),str(d),'96'],capture_output=True,text=True,timeout=60)
                self.assertEqual(result.returncode,0,result.stderr)
                report=json.loads(result.stdout)
                self.assertEqual(report['status'],226);self.assertEqual(report['keyons'],[])
                self.assertTrue(report['source_unmodified']);self.assertTrue(report['cache_guards_equal'])
                self.assertTrue(report['reset_equal']);self.assertTrue(report['restore_equal'])
                self.assertEqual(report['events'],[]);self.assertEqual(report['pcm']['nonzero_frames'],0)

    def test_observation_pins_overwritten_pitch(self):
        for case in CASES:
            r=observe(io.StringIO(trace(case)),case)
            self.assertEqual(len(r['note_gates']),12)
            self.assertEqual(len(r['boundary_pitch_writes']),2 if case=='end-3-note' else 1)
            bad=copy.deepcopy(r);bad['boundary_pitch_writes']=[]
            with self.assertRaises(ValueError):contract(bad,case)
            if case=='end-3-note':
                bad=copy.deepcopy(r);bad['boundary_pitch_writes'].reverse()
                with self.assertRaises(ValueError):contract(bad,case)

    def test_bounded_and_malformed_trace_reject(self):
        for text in ('invalid\n',trace('end-2-note').replace(',76,4\n',',76,12\n',1),trace('end-3-note').replace(',35,6\n',',35,5\n',1),'x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'end-3-note')

    def test_complete_reference_matrix_and_faults(self):
        candidates={f'{c}-{a}':native(PROBE,bank(a,c)) for c in QUALIFIED_CASES for a in (63,127)}
        refs=[]
        for c in CASES:
            for a in (63,127):
                r=observe(io.StringIO(trace(c)),c)
                if c in QUALIFIED_CASES:
                    n=candidates[f'{c}-{a}'];gates,rs=native_observation(n)
                    r['note_gates']=[{'voice':g['voice'],'onset_index':g['onset_index'],'gate_spc_cycles':int(g['gate_spc_cycles']),'release_kind':'keyoff'} for g in gates]
                    r['onset_intervals_spc_cycles']=[int((b['half_cycle']-a['half_cycle'])/2) for a,b in zip(n['keyons'],n['keyons'][1:])]
                for m in ('sgb','sgb2'):refs.append({**copy.deepcopy(r),'case':c,'articulation':a,'model':m})
        self.assertEqual(len(compare(candidates,refs)),8)
        for mutation in ('gate','duplicate','type','pitch'):
            bad=copy.deepcopy(refs)
            if mutation=='gate':bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
            elif mutation=='duplicate':bad[-1]=bad[0]
            elif mutation=='type':bad[0]['note_gates'][0]['voice']=True
            else:bad[8]['boundary_pitch_writes']=[]
            with self.assertRaises(ValueError):compare(candidates,bad)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
