#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Delayed authored-score controls, strict corpus scope and opaque reference API."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import check_sgb_vendor_delayed_tempo_reference as reference
from build_sgb_vendor_tick_selection_fixture import build
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_tick_reference import expected
from check_sgb_vendor_selection_wait_reference import WAITS
import sgb_vendor_selection_wait_tests as wait_tests

TIMELINE=PROBE=None
TIME_FIELDS={'half','first','last','first_read','last_read','first_equal','last_equal',
             'first_reply','last_reply','first_equal_reply','last_equal_reply','first_music_reply',
             'keyon_half','completion_half','end_half','directory_half','root_half'}


def synthetic(live=32,duration=4,position='between',wait=None):
    data=wait_tests.SelectionWaitContracts().handoff_timeline(live,position)
    factor=duration//2
    def stretch(value):
        if isinstance(value,dict):
            return {k:v*factor if k in TIME_FIELDS|{'min_gap','max_gap'} and type(v) is int else
                    [t*factor for t in v] if k=='requests' and all(type(t) is int for t in v) else stretch(v)
                    for k,v in value.items()}
        if isinstance(value,list):return [stretch(v) for v in value]
        return value
    data=stretch(data)
    directory=data['selection_timer_reads'][1]['directory_half']
    root=data['selection_timer_reads'][1]['root_half']
    delta=int((WAITS[live] if wait is None else wait)*2048)-(root-directory)
    def shift(value):
        if isinstance(value,dict):
            return {k:v+delta if k in TIME_FIELDS and type(v) is int and v>=root else shift(v) for k,v in value.items()}
        if isinstance(value,list):return [shift(v) for v in value]
        return value
    return shift(data)


class DelayedTempoContracts(unittest.TestCase):
    def test_fixture_layout_duration_and_defaults(self):
        self.assertEqual(len(reference.cases()),96)
        for duration in (2,4,8,16):
            image=build(128,live_tempo=32,change_after=3,boundary=True,request_gap=10,prefix_duration=duration)
            header=bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,128,duration,125,0xA4,0xA3,0xA4,0xE7,32))
            start=image.index(header)
            self.assertEqual(start,0x4000+4+0x60)
            self.assertEqual(image[start+16],32)  # 2B60 + 16 = fixed 2B70
            self.assertEqual(int.from_bytes(image[0x14E:0x150],'big'),(sum(image[:0x14E])+sum(image[0x150:]))&65535)
            baseline=build(128,live_tempo=32,change_after=3,boundary=True,request_gap=10)
            changes=[i for i,(a,b) in enumerate(zip(image,baseline)) if a!=b and i not in (0x14E,0x14F)]
            self.assertEqual(changes,[] if duration==2 else [start+10])
        for flags in (dict(prefix_duration=True),dict(prefix_duration=3),dict(prefix_duration=0)):
            with self.assertRaises(ValueError):build(128,live_tempo=32,change_after=3,boundary=True,**flags)
        with self.assertRaises(ValueError):build(prefix_duration=4)
        for duration in (True,0,97):
            with self.assertRaises(ValueError):expected(128,0,duration=duration)
        self.assertEqual(expected(128,0,duration=8)[0],65536)

    def test_ordering_and_predictions_require_observed_crossing(self):
        for duration in reference.GAPS:
            data=synthetic(duration=duration)
            result=reference.summarize(data,32,duration)
            self.assertTrue(result['unique_publication_crossing'])
            self.assertEqual(result['timing_evidence'],'later_tempo_only')
            starting=reference.summarize(synthetic(duration=duration,wait=20),32,duration)
            self.assertEqual(starting['timing_evidence'],'starting_tempo_only')
            control=reference.summarize(synthetic(128,duration),128,duration)
            self.assertEqual(control['timing_evidence'],'control_agrees')
            outside=reference.summarize(synthetic(duration=duration,position='before'),32,duration)
            self.assertFalse(outside['publication_crossing'])
            self.assertEqual(outside['timing_evidence'],'not_distinguished')
            with self.assertRaises(ValueError):reference.summarize(data,32,2)
            with self.assertRaises(ValueError):reference.summarize(data,32,True)
            bad=copy.deepcopy(data);bad['tempo_operand_reads'][0]['address']=0x2B71
            with self.assertRaises(ValueError):reference.summarize(bad,32,duration)
            bad=copy.deepcopy(data);bad['tempo_operand_reads']*=2
            ambiguous=reference.summarize(bad,32,duration)
            self.assertFalse(ambiguous['unique_publication_crossing'])
            self.assertEqual(ambiguous['timing_evidence'],'not_distinguished')
            for fault in ('binding','gate','directory','timer'):
                bad=copy.deepcopy(data)
                if fault=='binding':next(e for e in bad['events'] if e['register']==0x4C)['srcn']=3
                elif fault=='gate':bad['events']=[e for e in bad['events'] if e['register']!=0x5C or e['value']!=4]
                elif fault=='directory':bad['selection_directory_reads'].pop()
                else:bad['timer_configuration']['writes']=[dict(half=1)]
                with self.assertRaises((ValueError,StopIteration)):reference.summarize(bad,32,duration)

    def test_repeated_publications_keep_the_first_write_hypothesis_separate(self):
        data=synthetic()
        operand=data['tempo_operand_reads'][0]['half']
        # Two chronological candidates, with only the later one receiving replies.
        for key in ('port_handshakes','spc_reply_handshakes','handoff_writes'):
            data[key].append(copy.deepcopy(data[key][-1]))
            early=data[key][-2];late=data[key][-1]
            late['half']=operand+20
            if key=='port_handshakes':
                early.update(first_read=0,last_read=0,first_equal=0,last_equal=0,equal_reads=0,unequal_reads=0)
                late.update(first_read=operand+61,last_read=operand+61,first_equal=operand+61,last_equal=operand+61)
            elif key=='spc_reply_handshakes':
                early.update(first_reply=0,last_reply=0,first_equal_reply=0,last_equal_reply=0,reply_writes=0,equal_reply_writes=0)
                late.update(first_reply=operand+60,last_reply=operand+60,first_equal_reply=operand+60,last_equal_reply=operand+60)
            else:
                early.update(first_music_reply=0,music_reply_writes=0)
                late.update(first_music_reply=operand+60)
        result=reference.summarize(data,32,4)
        self.assertEqual(result['publication_order'],'writes_straddle_operand')
        self.assertEqual(result['handoff']['category'],'ambiguous_music_writes')
        self.assertEqual(len(result['handoff']['music_writes']),2)
        self.assertFalse(result['unique_publication_crossing'])
        self.assertEqual(result['timing_evidence'],'not_distinguished')
        self.assertTrue(result['first_publication_before_operand'])
        self.assertEqual(result['first_publication_evidence'],'later_tempo_only')
        absent=synthetic()
        old=[e for e in absent['events'] if e['register']==0x4C][:-1]
        absent['events']=[e for e in absent['events'] if all(e is not n for n in old[3:])]
        absent['tempo_operand_reads']=[]
        missing=reference.summarize(absent,32,4)
        self.assertEqual(missing['publication_order'],'operand_not_observed')
        self.assertFalse(missing['first_publication_before_operand'])
        self.assertEqual(missing['first_publication_evidence'],'not_distinguished')
        # Equal timestamps and ambiguous operand reads cannot prove the first-write ordering.
        for change in ('equal','ambiguous'):
            bad=copy.deepcopy(data)
            if change=='equal':bad['tempo_operand_reads'][0]['half']=bad['handoff_writes'][-2]['half']
            else:bad['tempo_operand_reads']*=2
            result=reference.summarize(bad,32,4)
            self.assertFalse(result['first_publication_before_operand'])
            self.assertEqual(result['first_publication_evidence'],'not_distinguished')

    def test_corpus_scope_and_sanitized_failures(self):
        runs=[dict(model=m,live=l,duration=d,gap=g,spin=s,**reference.summarize(synthetic(l,d),l,d))
              for m,l,d,g,s in reference.cases()]
        comparison=reference.compare(runs)
        self.assertEqual(len(comparison['pairs']),48)
        self.assertEqual(comparison['distinguishing_pairs'],48)
        self.assertFalse(comparison['parameters_fitted'])
        for bad in (runs[:-1],runs+[runs[0]],runs[:-1]+[runs[0]]):
            with self.assertRaises(ValueError):reference.compare(bad)
        for field,value in (('duration',True),('wait_ms',float('nan')),('unique_publication_crossing',1)):
            bad=copy.deepcopy(runs);bad[0][field]=value
            with self.assertRaises(ValueError):reference.compare(bad)
        def capture(probe,program,game,model,clocks,inputs,**flags):
            _,live,duration,gap,spin=game.stem.split('-');live,duration,gap,spin=map(int,(live,duration,gap,spin))
            self.assertEqual(game.read_bytes(),build(128,live_tempo=live,change_after=3,boundary=True,
                request_gap=gap,selection_spin=spin,prefix_duration=duration))
            self.assertEqual(clocks,300000000);self.assertEqual(flags,dict(tempo_publication=True))
            return synthetic(live,duration)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom','probe'):(root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root)]
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,96)
            self.assertEqual(report['schema'],'gbb-sgb-vendor-delayed-tempo-v1')
            self.assertIs(report['qualification'],False);self.assertIs(report['parameters_fitted'],False)
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised:reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded delayed-tempo experiment failed; child diagnostics suppressed\n')

    def test_native_controls(self):
        if TIMELINE is None:self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                model,live,duration,gap,spin=case;game=root/f'{model}-{live}-{duration}-{gap}-{spin}.gb'
                game.write_bytes(build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,
                                       selection_spin=spin,prefix_duration=duration))
                data=reference.capture(TIMELINE,program,game,model,300000000,inputs,tempo_publication=True)
                return dict(model=model,live=live,duration=duration,gap=gap,spin=spin,**reference.summarize(data,live,duration))
            with ThreadPoolExecutor(max_workers=4) as pool:runs=list(pool.map(run,reference.cases()))
            self.assertEqual(len(reference.compare(runs)['pairs']),48)

    def test_native_transparency_and_replay(self):
        if TIMELINE is None or PROBE is None:self.skipTest('both probes not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                model,live,duration=case;game=root/f'{model}-{live}-{duration}.gb'
                game.write_bytes(build(128,live_tempo=live,change_after=3,boundary=True,
                    request_gap=reference.GAPS[duration][model][1],prefix_duration=duration))
                captures=[reference.capture(TIMELINE,program,game,model,300000000,inputs,**flags)
                          for flags in (dict(tempo_boundary=True),dict(tempo_publication=True))]
                self.assertEqual(*captures);reference.summarize(captures[1],live,duration)
                results=[]
                for mode in ('native','scalar'):
                    child=subprocess.run([str(PROBE),str(program),str(game),model,'100000000',mode,'bundled'],capture_output=True,timeout=180)
                    self.assertEqual(child.returncode,0,child.stderr);self.assertLess(len(child.stdout),8192)
                    data=json.loads(child.stdout)
                    self.assertTrue(data['reset_equal']);self.assertTrue(data['restore_equal'])
                    for key in ('selection_wait_restores','selection_wait_pulse_restores','unread_restores','nonzero'):self.assertGreater(data[key],0)
                    self.assertEqual((data['starts'],data['completes'],data['selected'],data['rejected']),(2,1,2,0))
                    results.append(data)
                for key in ('pcm_fnv64','frames','clocks','last_nonzero_clock','nonzero','notes'):self.assertEqual(results[0][key],results[1][key])
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(run,[(m,l,d) for d in reference.GAPS for m in reference.GAPS[d] for l in (32,128)]))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path);parser.add_argument('--probe',type=Path)
    args,rest=parser.parse_known_args();TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
