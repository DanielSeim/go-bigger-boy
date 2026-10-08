#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual E0 writes, bounded prefixes, timer backlog and full lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import sgb_score_sparse_tests as sparse
from build_sgb_score_reselect import build,source
from build_sgb_score_sparse import build as prior_build
from build_sgb_reselect_fixture import bank
from check_sgb_score_reselect_reference import native,align,validate,compare,observe,write_groups,CASES
from check_sgb_score_polygate_reference import native as gate_native,native_gates
from build_sgb_prototype import assemble

for module in (sparse,sparse.lists,sparse.lists.banks,sparse.lists.banks.envelope,sparse.lists.banks.envelope.mix):
    module.native,module.align,module.validate,module.compare=native,align,validate,compare


def trace(case):
    text=sparse.trace('transitions').splitlines(keepends=True)
    # The sparse trace helper writes setup before every note. Replace only
    # instrument registers with the independently pinned E0 group sequence.
    rows=[];groups=write_groups(case);index=0
    for row in text:
        fields=row.strip().split(',')
        if fields[0]!='D':rows.append(row);continue
        address,value=int(fields[4]),int(fields[5])
        if address in (0x24,0x25,0x26,0x27,0x34,0x35,0x36,0x37):continue
        if address==76 and value:
            for register,val in groups[index]:rows.append(f'D,0,{fields[2]},0,{register},{val}\n')
            index+=1
        rows.append(row)
    return ''.join(rows)


class ReselectTests(sparse.SparseTests):
    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build())
        self.assertEqual(len(build()),3597)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'879bb92d5633788c11c6afc3c34f575a124b205e2f65a3aa97c216eb43b60277')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'a95e890efb333306c0c87b17e742eaee51178eab498b1973ddc436774c37a187')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_repeated_prefix_writes_and_all_gate_profiles(self):
        for tempo,art,duration in sparse.PULSES:
            for count in (1,3,5):
                note=(*([0xE0,2]*count),duration,art,0x98,0xA4,0)
                data=sparse.authored([(note,note),(note,None),(None,note),(note,note)])
                report=native(sparse.lists.banks.envelope.mix.PROBE,data,tempo);align(report,data)
                self.assertEqual(len(report['instrument_writes']),24*count)
                self.assertEqual([event['instrument_sets'] for event in report['events']],
                                 [count,count,0,0,count,0,count,0,count,count,0,0])
                self.assertEqual(report['pattern_ticks'],[0,2*duration,4*duration,6*duration])

    def test_full_mask_cache_prefix_writes_and_count_bound(self):
        note=(*([0xE0,2]*5),16,63,*([0x98]*8),0)
        report=self.render([(note,note)]*4)
        self.assertEqual(len(report['events']),64)
        self.assertEqual(len(report['instrument_writes']),160)
        bad=(*([0xE0,2]*6),16,63,0x98,0)
        self.reject(sparse.authored([((16,63,0x98,0),None),(None,bad)]))

    def test_rest_prefix_updates_only_selected_voice_without_kon(self):
        rest=(0xE0,2,0xE0,2,16,63,0xC9,0)
        for pair,voice in (((rest,None),2),((None,rest),3)):
            report=self.render([pair]*4)
            self.assertEqual(len(report['instrument_writes']),32)
            self.assertTrue(all(write['register']//16==voice for write in report['instrument_writes']))
            self.assertEqual(report['keyons'],[])
            self.assertEqual(report['pcm']['peak'],0)

    def test_actual_write_metadata_and_wrong_driver_fail(self):
        data=bank(63,'reselect-3');report=native(sparse.lists.banks.envelope.mix.PROBE,data);align(report,data)
        for key,value in (('register',0x34),('value',0),('half_cycle',0),('half_cycle',True),('held_mask',0)):
            changed=copy.deepcopy(report);changed['instrument_writes'][0][key]=value
            with self.assertRaises(ValueError):validate(changed)
        for writes in ([],report['instrument_writes'][:-1],report['instrument_writes']+[report['instrument_writes'][0]]):
            changed=copy.deepcopy(report);changed['instrument_writes']=writes
            with self.assertRaises(ValueError):validate(changed)
        changed=copy.deepcopy(report);changed['events'][0]['instrument_sets']=True
        with self.assertRaises(ValueError):validate(changed)
        old=gate_native(sparse.lists.banks.envelope.mix.PROBE,data,builder=prior_build,validator=lambda _:None,output_bound=131072)
        with self.assertRaises(ValueError):align(old,data)
        # Register snapshots alone cannot detect a skipped same-value write;
        # accepted CPU write observations must reject that omission.
        damaged=source().replace('    mov a, $1540+x\n    mov $f3, a\n','    mov a, $1540+x\n')
        with self.assertRaises(ValueError):
            gate_native(sparse.lists.banks.envelope.mix.PROBE,data,builder=lambda:bytes(assemble(damaged,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_timer_backlog_is_not_charged_to_new_note(self):
        data=bank(127,'reselect-3')
        report=native(sparse.lists.banks.envelope.mix.PROBE,data);align(report,data)
        # Removing backlog exclusion reproduces an actual too-short gate,
        # rather than merely disagreeing with an implementation counter.
        damaged=source().replace('gates_on:\n    call reselect_pending\n','gates_on:\n')
        with self.assertRaises(ValueError):
            gate_native(sparse.lists.banks.envelope.mix.PROBE,data,builder=lambda:bytes(assemble(damaged,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_reselection_reference_observer_and_matrix_guards(self):
        candidates={f'{case}-{art}':native(sparse.lists.banks.envelope.mix.PROBE,bank(art,case)) for case in CASES for art in (127,63)}
        refs=[]
        for case in CASES:
            for art in (127,63):
                report=observe(io.StringIO(trace(case)),case)
                report['note_gates']=[{'voice':edge['voice'],'release_kind':'keyoff','gate_spc_cycles':int(edge['gate_spc_cycles'])} for edge in native_gates(candidates[f'{case}-{art}'])]
                refs.extend({**copy.deepcopy(report),'case':case,'articulation':art,'model':model} for model in ('sgb','sgb2'))
            missing=trace(case).replace(',0,39,184\n',',0,40,184\n')
            with self.assertRaises(ValueError):observe(io.StringIO(missing),case)
        self.assertEqual(len(compare(candidates,refs)),8)
        for altered in (None,refs[:-1],refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):compare(candidates,altered)
        for key in ('writes','type','gate','model'):
            altered=copy.deepcopy(refs)
            if key=='writes':altered[0]['instrument_writes_before_onsets'][2]=[]
            if key=='type':altered[0]['instrument_writes_before_onsets'][2][0][1]=True
            if key=='gate':altered[0]['note_gates'][0]['gate_spc_cycles']+=6000
            if key=='model':altered[-1]['model']='sgb'
            with self.assertRaises(ValueError):compare(candidates,altered)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    sparse.lists.banks.envelope.mix.PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
