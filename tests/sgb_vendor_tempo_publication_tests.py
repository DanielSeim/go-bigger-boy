#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned controls for the publication sweep, including overlapping startup."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import check_sgb_vendor_tempo_publication_reference as reference
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_tick_selection_fixture import build
import sgb_vendor_selection_wait_tests as wait_tests

TIMELINE=PROBE=None


def synthetic(live=32,overlap=False):
    data=wait_tests.SelectionWaitContracts().handoff_timeline(live)
    if not overlap:return data
    notes=[e for e in data['events'] if e['register']==0x4C];old=notes[:2];selected=notes[-1]
    request=old[0]['half']-10000;data['requests'][1]=request
    data['events']=[e for e in data['events'] if e['half']<old[1]['half'] or e is old[1] or e['half']>=selected['half']]
    data['events'].extend([dict(half=old[1]['half']+1000,register=0x5C,value=255,request=2),
                           dict(half=old[1]['half']+1020,register=0x5C,value=0,request=2)])
    data['events'].sort(key=lambda e:e['half'])
    for e in data['events']:
        if e['half']>=request:e['request']=2
    for r in data['selection_traversal_reads']:
        if r['request']==1:r['half']+=34900;r['request']=2
    for key in ('selection_directory_reads','selection_traversal_reads'):
        data[key].sort(key=lambda r:(r['half'],r['request']))
    data['selection_timer_reads']=data['selection_timer_reads'][1:]
    data['timer_continuity']=data['timer_continuity'][1:]
    data['tempo_operand_reads']=[]
    half=old[1]['half']+100;answer=half+10
    data['port_handshakes'][-1].update(half=half,first_read=answer+1,last_read=answer+1,first_equal=answer+1,last_equal=answer+1)
    data['spc_reply_handshakes'][-1].update(half=half,first_reply=answer,last_reply=answer,first_equal_reply=answer,last_equal_reply=answer)
    data['handoff_writes'][-1].update(half=half,first_music_reply=answer)
    return data


class PublicationContracts(unittest.TestCase):
    def test_scope_ordering_and_startup(self):
        self.assertEqual(len(reference.cases()),56)
        for model,live,gap,spin in reference.cases():
            image=build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,selection_spin=spin)
            baseline=build(128,live_tempo=live,change_after=3,boundary=True,request_gap=8)
            self.assertEqual(image[0x4000:],baseline[0x4000:])
        for position,order in (('before','all_writes_after_operand'),('between','all_writes_before_operand'),
                               ('after','all_writes_before_operand'),('shared','all_writes_before_operand')):
            data=wait_tests.SelectionWaitContracts().handoff_timeline(position=position)
            self.assertEqual(reference.summarize(data,32)['publication_order'],order)
        startup=synthetic(overlap=True);summary=reference.summarize(startup,32)
        self.assertIs(summary['startup_overlap'],True);self.assertEqual(summary['old_note_requests'],[2,2])
        self.assertEqual(summary['publication_order'],'operand_not_observed')
        self.assertEqual(summary['handoff']['category'],'operand_not_observed')
        self.assertEqual(len(startup['timer_continuity']),1)
        for fault in ('binding','read','release','gate','timer','snapshot','directory','traversal'):
            bad=copy.deepcopy(startup)
            if fault=='binding':next(e for e in bad['events'] if e['register']==0x4C)['pitch']=1357
            elif fault=='read':bad['tempo_operand_reads']=[dict(half=bad['requests'][1]+1,address=0x2B70,request=2)]
            elif fault=='release':bad['events']=[e for e in bad['events'] if e['register']!=0x5C or e['value']!=255]
            elif fault=='gate':bad['events']=[e for e in bad['events'] if e['register']!=0x5C or e['value']!=4]
            elif fault=='timer':bad['timer_configuration']['writes']=[dict(half=1)]
            elif fault=='snapshot':bad['timer_configuration']['requests'][0]['initial'][0]=2
            elif fault=='directory':bad['selection_directory_reads'].pop()
            else:bad['selection_traversal_reads'].pop()
            with self.assertRaises((ValueError,StopIteration)):reference.summarize(bad,32)
        runs=[dict(model=m,live=l,gap=g,spin=s,**reference.summarize(synthetic(l,overlap=bool(s)),l)) for m,l,g,s in reference.cases()]
        compared=reference.compare(runs);self.assertEqual(len(compared['pairs']),28)
        self.assertIs(compared['parameters_fitted'],False)
        for bad in (runs[:-1],runs+[runs[0]],runs[:-1]+[runs[0]]):
            with self.assertRaises(ValueError):reference.compare(bad)
        for field,value in (('spin',False),('wait_ms',float('nan')),('startup_overlap',1)):
            bad=copy.deepcopy(runs);bad[0][field]=value
            with self.assertRaises(ValueError):reference.compare(bad)
        for flags in (dict(request_gap=1),dict(selection_spin=-1),dict(selection_spin=2500),dict(selection_spin=True)):
            with self.assertRaises(ValueError):build(128,live_tempo=32,change_after=3,boundary=True,**flags)

    def test_reference_corpus_and_failure(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            _,live,gap,spin=game.stem.split('-');live=int(live)
            self.assertEqual(game.read_bytes(),build(128,live_tempo=live,change_after=3,boundary=True,request_gap=int(gap),selection_spin=int(spin)))
            self.assertEqual(clocks,300000000);self.assertEqual(flags,dict(tempo_publication=True))
            return synthetic(live,overlap=not int(spin))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'):(root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root)]
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,56)
            self.assertEqual(report['schema'],'gbb-sgb-vendor-tempo-publication-v1')
            self.assertIs(report['qualification'],False);self.assertIs(report['parameters_fitted'],False)
            self.assertEqual({r['startup_overlap'] for r in report['runs']},{True,False})
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised:reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded tempo-publication experiment failed; child diagnostics suppressed\n')

    def test_native_controls(self):
        if TIMELINE is None:self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                model,live,gap,spin=case;game=root/f'{model}-{live}-{gap}-{spin}.gb'
                game.write_bytes(build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,selection_spin=spin))
                data=reference.capture(TIMELINE,program,game,model,300000000,inputs,tempo_publication=True)
                return dict(model=model,live=live,gap=gap,spin=spin,**reference.summarize(data,live))
            with ThreadPoolExecutor(max_workers=4) as pool:runs=list(pool.map(run,reference.cases()))
            self.assertEqual(len(reference.compare(runs)['pairs']),28)

    def test_native_transparency(self):
        if TIMELINE is None:self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                model,live=case;game=root/f'{model}-{live}.gb'
                game.write_bytes(build(128,live_tempo=live,change_after=3,boundary=True,request_gap=reference.GAPS[model][0]))
                old=reference.capture(TIMELINE,program,game,model,300000000,inputs,tempo_boundary=True)
                new=reference.capture(TIMELINE,program,game,model,300000000,inputs,tempo_publication=True)
                reference.summarize(new,live);self.assertEqual(old,new)
            with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(run,[(m,l) for m in reference.GAPS for l in (32,128)]))

    def test_native_replay(self):
        if PROBE is None:self.skipTest('playback probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            def run(case):
                model,live=case;game=root/f'{model}-{live}.gb'
                game.write_bytes(build(128,live_tempo=live,change_after=3,boundary=True,request_gap=reference.GAPS[model][0]))
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
            with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(run,[(m,l) for m in reference.GAPS for l in (32,128)]))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path);parser.add_argument('--probe',type=Path)
    args,rest=parser.parse_known_args();TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
