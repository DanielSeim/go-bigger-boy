#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in root pacing, direct error gates and interruptible snapshot continuation."""
import argparse
import copy
from concurrent.futures import ThreadPoolExecutor
import hashlib
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_selection_wait_fixture import build,TEMPOS
from check_sgb_vendor_selection_wait_reference import compare,waits,WAITS
import check_sgb_vendor_selection_wait_reference as reference
import check_sgb_vendor_selection_phase_reference as phase
import check_sgb_vendor_timer_tempo_reference as timer_tempo
import check_sgb_vendor_accumulator_model as accumulator_model
from build_sgb_vendor_tick_fixture import build as tick_fixture
import check_sgb_vendor_tick_reference as tick_reference
from build_sgb_vendor_tick_selection_fixture import build as tick_selection_fixture
import check_sgb_vendor_tick_selection_reference as tick_selection_reference
import check_sgb_vendor_timer_writes_reference as timer_write_reference
import check_sgb_vendor_tempo_change_reference as tempo_change_reference

TIMELINE=PROBE=INTERRUPT=None


def tick_timeline(tempo=45):
    start=100000;times=[0,*tick_reference.expected(tempo,0)]
    events=[]
    for i,pitch in enumerate([1437,1357]*4):
        onset=start+times[i];gate=start+(times[i]+times[i+1])//2
        events.extend([dict(half=onset,register=0x4C,value=4,request=1,pitch=pitch,
                           srcn=2,adsr1=255,adsr2=224,gain=184),
                       dict(half=gate,register=0x5C,value=4,request=1),
                       dict(half=gate+20,register=0x5C,value=0,request=1)])
    completion=start+times[-1]+100
    events.extend([dict(half=completion,register=0x5C,value=255,request=1),
                   dict(half=completion+20,register=0x5C,value=0,request=1)])
    def registers(low,high):
        stats=dict(count=2,first=low,last=high,min_gap=high-low,max_gap=high-low)
        empty=dict(count=0,first=0,last=0,min_gap=0,max_gap=0)
        return [dict(address=a,all=stats.copy() if a==253 else empty.copy(),
                     nonzero=stats.copy() if a==253 else empty.copy()) for a in range(253,256)]
    return dict(music=[1],requests=[50000],events=events,selection_timer_results=True,
        timer_continuity=[dict(request=1,keyon_half=start,completion_half=completion,end_half=1000000,
            playback=registers(start+100,completion-100),idle=registers(completion+100,999900))])


def tick_selection_timeline(tempo=45,state='active',start_tempo=None):
    original=tick_timeline(tempo);events=original['events'][:13]
    events[-1]['pitch']=1281
    if start_tempo is not None:
        delta=tick_reference.expected(start_tempo,0)[0]-tick_reference.expected(tempo,0)[0]
        for event in events[3:]: event['half']+=delta
        events[1]['half']=(events[0]['half']+events[3]['half'])//2
        events[2]['half']=events[1]['half']+20
    start=events[0]['half'];tail=events[-1]['half']
    completion=tail+96*256*4096//tempo if state=='completed' else 0
    request=(completion or tail)+50000;directory=request+100;root=directory+32768;onset=root+1000
    if completion: events.append(dict(half=completion,register=0x5C,value=255,request=1))
    selected={**events[0],'half':onset,'request':2,'pitch':1357};events.append(selected)
    end=onset+200000;events.append(dict(half=end,register=0x5C,value=255,request=2))
    def registers(low,high):
        stats=dict(count=2,first=low,last=high,min_gap=high-low,max_gap=high-low)
        empty=dict(count=0,first=0,last=0,min_gap=0,max_gap=0)
        return [dict(address=a,all=stats.copy() if a==253 else empty.copy(),
                     nonzero=stats.copy() if a==253 else empty.copy()) for a in range(253,256)]
    # Keep exactly FD/FE/FF when an aggregate stage is absent.
    absent=lambda:[dict(address=a,all=dict(count=0,first=0,last=0,min_gap=0,max_gap=0),
                        nonzero=dict(count=0,first=0,last=0,min_gap=0,max_gap=0)) for a in range(253,256)]
    windows=[dict(request=1,keyon_half=start,completion_half=completion,end_half=request,
                  playback=registers(start+100,(completion or request)-10000),
                  idle=registers(completion+100,request-10000) if completion else absent()),
             dict(request=2,keyon_half=onset,completion_half=end,end_half=end+1000,
                  playback=registers(onset+100,end-100),idle=registers(end+100,end+900))]
    dirs=[];traversal=[];timers=[]
    for i,note,d,r in ((0,events[0],60000,60100),(1,selected,directory,root)):
        dirs.extend(dict(request=i+1,half=d-j,address=0x2B00+2*i+j) for j in (0,1))
        for base,addresses in ((r,(0x2B10+4*i,0x2B11+4*i)),
                               (r+100,(0x2B24+16*i,0x2B25+16*i)),
                               (r+200,(0x2B60+48*i,0x2B6C+48*i))):
            traversal.extend(dict(request=i+1,half=base+j,address=a) for j,a in enumerate(addresses))
        def accesses():
            return [dict(address=a,count=0,first=0,last=0,min_gap=0,max_gap=0,nonzero=[]) for a in range(253,256)]
        timers.append(dict(request=i+1,directory_half=d,root_half=r,wait=accesses(),setup=accesses()))
    return dict(music=[1,2],requests=[50000,request],events=events,selection_timer_results=True,
                timer_continuity=windows,selection_directory_reads=dirs,
                selection_traversal_reads=traversal,selection_timer_reads=timers)


class SelectionWaitContracts(unittest.TestCase):
    def test_tempo_change_scope_witnesses_and_contrasts(self):
        runs=[]
        for model in ('sgb','sgb2'):
            for start in tempo_change_reference.TEMPOS:
                for live in tempo_change_reference.TEMPOS:
                    for state in ('active','completed'):
                        for holdout in (False,True):
                            image=tick_selection_fixture(start,state,holdout=holdout,live_tempo=live)
                            self.assertEqual(image,tick_selection_fixture(start,state,holdout=holdout,live_tempo=live))
                            self.assertIn(bytes((2,125,0xA4,0xE7,live,0xA3,0xA4,0xA3,96,0xA2,0)),image)
                            other=tick_selection_fixture(start,'completed' if state=='active' else 'active',holdout=not holdout,live_tempo=live)
                            self.assertEqual(image[0x4000:],other[0x4000:])
                            data=self.timer_configuration(tick_selection_timeline(live,state,start))
                            measured=tempo_change_reference.summarize(data,start,live,state)
                            self.assertEqual(measured['prefix_max_error_ms'],0)
                            runs.append(dict(model=model,start_tempo=start,live_tempo=live,state=state,holdout=holdout,
                                             observed=WAITS[live]*2048))
        compared=tempo_change_reference.compare(runs)
        self.assertIs(compared['parameters_fitted'],False)
        self.assertTrue(all(c['live_within_2ms'] and not c['starting_within_2ms'] for c in compared['cases']))
        changed=copy.deepcopy(runs)
        for row in changed:
            if row['holdout'] and row['start_tempo']!=row['live_tempo']: row['observed']+=16384
        after=tempo_change_reference.compare(changed)
        self.assertEqual([c for c in compared['cases'] if not c['holdout']],[c for c in after['cases'] if not c['holdout']])
        self.assertTrue(all(not c['live_within_2ms'] for c in after['cases'] if c['holdout']))
        for bad in (runs[:-1],runs+[runs[0]],runs[:-1]+[runs[0]]):
            with self.assertRaises(ValueError): tempo_change_reference.compare(bad)
        for live in (True,99):
            with self.assertRaises(ValueError): tick_selection_fixture(live_tempo=live)
        data=self.timer_configuration(tick_selection_timeline(128,'active',32))
        with self.assertRaises(ValueError): tempo_change_reference.summarize(data,128,128,'active')
        with self.assertRaises(ValueError): tempo_change_reference.summarize(data,32,32,'active')
        data['events'][0]['pitch']=1357
        with self.assertRaises(ValueError): tempo_change_reference.summarize(data,32,128,'active')

    def test_tempo_change_reference_filters_and_failure(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            _,start,live,state,holdout=game.stem.split('-');start=int(start);live=int(live);holdout=bool(int(holdout))
            image=tick_selection_fixture(start,state,holdout=holdout,live_tempo=live)
            self.assertEqual(game.read_bytes(),image)
            self.assertEqual(clocks,300000000);self.assertEqual(flags,dict(timer_writes=True))
            return self.timer_configuration(tick_selection_timeline(live,state,start))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root)]
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(tempo_change_reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):
                tempo_change_reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,32)
            self.assertIs(report['qualification'],False)
            for row in report['runs']:
                self.assertEqual(row['fixture_sha256'],hashlib.sha256(tick_selection_fixture(row['start_tempo'],row['state'],holdout=row['holdout'],live_tempo=row['live_tempo'])).hexdigest())
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(tempo_change_reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: tempo_change_reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded tempo-change experiment failed; child diagnostics suppressed\n')

    def test_native_tempo_change_controls(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'owned.rom').write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,start,live,state,holdout) for model in ('sgb','sgb2')
                   for start in tempo_change_reference.TEMPOS for live in tempo_change_reference.TEMPOS
                   for state in ('active','completed') for holdout in (False,True)]
            def run(case):
                model,start,live,state,holdout=case;game=root/f'{model}-{start}-{live}-{state}-{int(holdout)}.gb'
                game.write_bytes(tick_selection_fixture(start,state,holdout=holdout,live_tempo=live))
                data=reference.capture(TIMELINE,root/'owned.rom',game,model,300000000,inputs,timer_writes=True)
                return dict(model=model,start_tempo=start,live_tempo=live,state=state,holdout=holdout,
                            **tempo_change_reference.summarize(data,start,live,state))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            for row in runs:
                self.assertLess(abs(row['observed']/2048-WAITS[row['live_tempo']]),.4)
            for row in tempo_change_reference.compare(runs)['cases']:
                self.assertTrue(row['live_within_2ms']);self.assertFalse(row['starting_within_2ms'])

    def test_native_tempo_change_replay(self):
        if PROBE is None: self.skipTest('playback probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            def run(case):
                model,start,live=case;game=root/f'{model}-{start}-{live}.gb'
                game.write_bytes(tick_selection_fixture(start,'active',holdout=True,live_tempo=live))
                results=[]
                for mode in ('native','scalar'):
                    child=subprocess.run([str(PROBE),str(program),str(game),model,'100000000',mode,'bundled'],
                                         capture_output=True,timeout=180)
                    self.assertEqual(child.returncode,0,child.stderr)
                    self.assertLess(len(child.stdout),8192);data=json.loads(child.stdout)
                    self.assertIs(data['reset_equal'],True);self.assertIs(data['restore_equal'],True)
                    self.assertGreater(data['selection_wait_restores'],0)
                    self.assertGreater(data['selection_wait_pulse_restores'],0)
                    self.assertGreater(data['unread_restores'],0);self.assertGreater(data['nonzero'],100)
                    self.assertEqual((data['starts'],data['completes'],data['notes'],data['selected'],data['rejected']),
                                     (2,1,6,2,0))
                    results.append(data)
                for key in ('pcm_fnv64','frames','clocks','last_nonzero_clock','nonzero'):
                    self.assertEqual(results[0][key],results[1][key])
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(run,[(m,a,b) for m in ('sgb','sgb2') for a,b in ((32,128),(128,32))]))

    def timer_configuration(self, data):
        initial=[1,16,-1,-1]
        data['timer_configuration']=dict(end_half=data['timer_continuity'][-1]['end_half'],
            boot_write_counts=[1,1,0,0],requests=[dict(request=i+1,initial=initial.copy())
            for i in range(len(data['requests']))],writes=[],final=initial.copy())
        return data

    def test_timer_configuration_bounds_and_replay(self):
        data=self.timer_configuration(tick_selection_timeline())
        self.assertEqual(timer_write_reference.configuration(data)['writes'],[])
        record=data['timer_configuration'];root=data['selection_timer_reads'][1]['root_half']
        record['writes']=[dict(half=root-10+j,request=2,address=a,value=v)
                          for j,(a,v) in enumerate(((241,1),(241,0),(241,1),(250,8),(250,16),(251,9)))]
        record['final']=[1,16,9,-1]
        measured=timer_write_reference.configuration(data)['requests'][1]['stages']['wait']
        self.assertEqual(measured,dict(writes=6,enable_rises=1,target_changes=2,target_initializations=1))
        faults=[]
        for key,value in (('address',242),('value',8),('request',True),('half',0)):
            bad=copy.deepcopy(data);bad['timer_configuration']['writes'][0][key]=value;faults.append(bad)
        for key,value in (('final',[1,16,-1,-1]),('boot_write_counts',[0,1,0,0]),('end_half',True)):
            bad=copy.deepcopy(data);bad['timer_configuration'][key]=value;faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_configuration']['requests'][1]['initial'][1]=8;faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_configuration']['writes'].reverse();faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_configuration']['writes']*=86;faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_configuration']['unexpected']=0;faults.append(bad)
        for bad in faults:
            with self.assertRaises(ValueError): timer_write_reference.configuration(bad)

    def test_timer_write_reference_filters_and_failure(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            state=game.stem.split('-')[-2];holdout=bool(int(game.stem.rsplit('-',1)[-1]))
            self.assertEqual(game.read_bytes(),tick_selection_fixture(64,state,holdout=holdout))
            self.assertEqual(clocks,300000000);self.assertEqual(flags,dict(timer_writes=True))
            return self.timer_configuration(tick_selection_timeline(64,state))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),'--tempo','64','--tempo','64']
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(timer_write_reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):
                timer_write_reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,8)
            self.assertIs(report['qualification'],False)
            for row in report['runs']:
                self.assertEqual(row['fixture_sha256'],hashlib.sha256(tick_selection_fixture(64,row['state'],holdout=row['holdout'])).hexdigest())
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(timer_write_reference,'capture',side_effect=fault,return_value=tick_selection_timeline(64)),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: timer_write_reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded timer-write experiment failed; child diagnostics suppressed\n')

    def test_native_timer_write_transparency(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'owned.rom').write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                model,state=case;game=root/f'{model}-{state}.gb'
                game.write_bytes(tick_selection_fixture(112,state,holdout=True))
                plain=reference.capture(TIMELINE,root/'owned.rom',game,model,300000000,inputs,timer_continuity=True)
                observed=reference.capture(TIMELINE,root/'owned.rom',game,model,300000000,inputs,timer_writes=True)
                config=timer_write_reference.configuration(observed)
                self.assertGreater(len(config['writes']),0)
                observed.pop('timer_configuration');self.assertEqual(plain,observed)
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(run,[(m,s) for m in ('sgb','sgb2') for s in ('active','completed')]))

    def test_tick_selection_witness_and_state(self):
        for tempo in timer_tempo.TEMPO_POINTS:
            for state in ('active','completed'):
                baseline=tick_selection_fixture(tempo,state);holdout=tick_selection_fixture(tempo,state,holdout=True)
                self.assertNotEqual(baseline,holdout);self.assertEqual(baseline[0x4000:],holdout[0x4000:])
                data=tick_selection_timeline(tempo,state)
                measured=tick_selection_reference.summarize(data,tempo,state)
                self.assertEqual(measured['prefix_max_error_ms'],0)
                self.assertIn(measured['phase'],measured['compatible_prefix_phases'])
                self.assertEqual(bool(measured['prior_completion_half']),state=='completed')
        for args in ((True,'active'),(45,'invalid'),(99,'active')):
            with self.assertRaises(ValueError): tick_selection_fixture(*args)
        with self.assertRaises(ValueError): tick_selection_fixture(45,holdout=1)
        data=tick_selection_timeline();faults=[]
        for key,value in (('pitch',1436),('request',2),('srcn',3)):
            bad=copy.deepcopy(data);bad['events'][0][key]=value;faults.append(bad)
        bad=copy.deepcopy(data);bad['events'].pop(1);faults.append(bad)
        bad=copy.deepcopy(data);bad['selection_traversal_reads'].pop();faults.append(bad)
        bad=copy.deepcopy(data);bad['selection_timer_reads'][1]['root_half']+=1;faults.append(bad)
        for bad in faults:
            with self.assertRaises(ValueError): tick_selection_reference.summarize(bad,45,'active')
        with self.assertRaises(ValueError): tick_selection_reference.summarize(data,45,'completed')

    def test_tick_selection_reference_fitting_and_failure(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            state=game.stem.split('-')[-2];holdout=bool(int(game.stem.rsplit('-',1)[-1]))
            self.assertEqual(game.read_bytes(),tick_selection_fixture(64,state,holdout=holdout))
            self.assertEqual(clocks,300000000);self.assertEqual(flags,dict(timer_continuity=True))
            return tick_selection_timeline(64,state)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),'--tempo','64','--tempo','64']
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(tick_selection_reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):
                tick_selection_reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,8)
            self.assertIs(report['qualification'],False)
            guard=report['comparison']['guard_opportunities']
            for row in report['runs']:
                predictions=[accumulator_model.predict(row,p,guard)/2048 for p in row['compatible_prefix_phases']]
                measured=next(c for c in report['comparison']['cases'] if all(c[k]==row[k] for k in ('model','state','holdout')))
                self.assertEqual(measured['compatible_prediction_range_ms'],[min(predictions),max(predictions)])
                self.assertEqual(measured['minimum_compatible_error_ms'],min(abs(p-row['observed']/2048) for p in predictions))
            for row in report['runs']:
                if row['holdout']: row['observed']+=16384
            changed=tick_selection_reference.compare(report['runs'])
            self.assertEqual(changed['guard_opportunities'],guard)
            self.assertFalse(changed['accuracy_passed'])
            self.assertEqual(changed['baseline_feasible_guards'],report['comparison']['baseline_feasible_guards'])
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(tick_selection_reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: tick_selection_reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded tick-selection experiment failed; child diagnostics suppressed\n')
        left=dict(model='sgb',tempo=32,state='active',holdout=False,keyon=10000,
                  anchor=20000,directory=24096,phase=0,prefix_max_error_ms=0,compatible_prefix_phases=[0])
        right={**left,'model':'sgb2'}
        left['observed']=accumulator_model.predict(left,0,4)
        right['observed']=accumulator_model.predict(right,0,12)
        impossible=tick_selection_reference.compare([left,right])
        self.assertEqual(impossible['baseline_feasible_guards'],[])
        self.assertFalse(impossible['accuracy_passed'])

    def test_native_tick_selection_controls(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'owned.rom').write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,tempo,state,holdout) for model in ('sgb','sgb2') for tempo in timer_tempo.TEMPO_POINTS
                   for state in ('active','completed') for holdout in (False,True)]
            def run(case):
                model,tempo,state,holdout=case;game=root/f'{model}-{tempo}-{state}-{int(holdout)}.gb'
                game.write_bytes(tick_selection_fixture(tempo,state,holdout=holdout))
                data=reference.capture(TIMELINE,root/'owned.rom',game,model,300000000,inputs,timer_writes=True)
                timer_write_reference.configuration(data)
                return case,tick_selection_reference.summarize(data,tempo,state)
            with ThreadPoolExecutor(max_workers=4) as pool:
                for (model,tempo,state,holdout),measured in pool.map(run,cases):
                    with self.subTest(model=model,tempo=tempo,state=state,holdout=holdout):
                        self.assertLessEqual(measured['prefix_max_error_ms'],2)
                        self.assertLess(abs(measured['observed']/2048-WAITS.get(tempo,0)),.4)

    def test_tick_fixture_and_music_ownership(self):
        for tempo in timer_tempo.TEMPO_POINTS:
            baseline=tick_fixture(tempo);holdout=tick_fixture(tempo,holdout=True)
            self.assertEqual(baseline,tick_fixture(tempo));self.assertNotEqual(baseline,holdout)
            self.assertEqual(baseline[0x4000:],holdout[0x4000:])
            measured=tick_reference.summarize(tick_timeline(tempo))
            self.assertEqual(measured['onsets_half'][1:],tick_reference.expected(tempo,0)[:-1])
            self.assertEqual(measured['gate_clear_half'],[20]*8)
        for tempo,holdout in ((True,False),(99,False),(32,1)):
            with self.assertRaises(ValueError): tick_fixture(tempo,holdout=holdout)
        data=tick_timeline();faults=[]
        final_clear=copy.deepcopy(data);final_clear['events'].pop(-3)
        self.assertIsNone(tick_reference.summarize(final_clear)['gate_clear_half'][-1])
        for key,value in (('pitch',1436),('srcn',3),('request',2)):
            bad=copy.deepcopy(data);bad['events'][0][key]=value;faults.append(bad)
        bad=copy.deepcopy(data);bad['events'].pop(1);faults.append(bad)
        bad=copy.deepcopy(data);bad['music']=[2];faults.append(bad)
        bad=copy.deepcopy(data);bad['events'].pop(-2);faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_continuity'][0]['completion_half']+=1;faults.append(bad)
        for bad in faults:
            with self.assertRaises(ValueError): tick_reference.summarize(bad)

    def test_tick_reference_filters_fit_and_sanitization(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            holdout=bool(int(game.stem.rsplit('-',1)[-1]))
            self.assertEqual(game.read_bytes(),tick_fixture(64,holdout=holdout))
            self.assertEqual(clocks,200000000);self.assertEqual(flags,dict(timer_continuity=True))
            return tick_timeline(64)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),'--tempo','64','--tempo','64']
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(tick_reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):
                tick_reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,4)
            self.assertIs(report['qualification'],False);self.assertEqual(len(report['runs']),4)
            before=report['comparisons']
            report['runs'][1]['onsets_half'][1]+=16384
            after=tick_reference.compare(report['runs'])
            self.assertEqual(before[0]['baseline_phase'],after[0]['baseline_phase'])
            self.assertEqual(before[0]['baseline_steady_phase'],after[0]['baseline_steady_phase'])
            self.assertFalse(after[0]['cases'][1]['onsets_within_2ms'])
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(tick_reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: tick_reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded music-tick experiment failed; child diagnostics suppressed\n')

    def test_native_repeated_music_ticks(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'owned.rom').write_bytes(firmware())
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,tempo,holdout) for model in ('sgb','sgb2')
                   for tempo in timer_tempo.TEMPO_POINTS for holdout in (False,True)]
            def run(case):
                model,tempo,holdout=case;game=root/f'{model}-{tempo}-{int(holdout)}.gb'
                game.write_bytes(tick_fixture(tempo,holdout=holdout))
                data=reference.capture(TIMELINE,root/'owned.rom',game,model,200000000,inputs,timer_continuity=True)
                return dict(model=model,tempo=tempo,holdout=holdout,**tick_reference.summarize(data))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            for pair in tick_reference.compare(runs):
                for case in pair['cases']:
                    with self.subTest(model=pair['model'],tempo=pair['tempo'],holdout=case['holdout']):
                        self.assertTrue(case['onsets_within_2ms'])
                        self.assertTrue(case['steady_onsets_within_2ms'])
                        self.assertLessEqual(case['completion_error_ms'],2)

    def test_persistent_accumulator_arithmetic(self):
        for tempo in timer_tempo.TEMPO_POINTS:
            for phase_value in (0,80,255):
                joined=accumulator_model.Accumulator(tempo,phase_value)
                split=accumulator_model.Accumulator(tempo,phase_value)
                carries=joined.advance(1027)
                self.assertEqual(carries,split.advance(27)+split.advance(1000))
                self.assertEqual(joined.phase,split.phase)
                self.assertEqual((carries,joined.phase),divmod(phase_value+1027*tempo,256))
                until=joined.until_carry()
                self.assertEqual(joined.advance(until),1)
        for tempo,seed in ((True,0),(0,0),(256,0),(32,True),(32,256)):
            with self.assertRaises(ValueError): accumulator_model.Accumulator(tempo,seed)
        for ticks in (-1,True,1.5):
            with self.assertRaises(ValueError): accumulator_model.Accumulator(32,0).advance(ticks)

    def test_accumulator_frozen_fit_and_failure(self):
        def corpus(schedule):
            runs=[]
            for model in ('sgb','sgb2'):
                for tempo in timer_tempo.TEMPO_POINTS:
                    for state in ('active','completed'):
                        case=dict(tempo=tempo,keyon=10000,anchor=20000,directory=24096)
                        observed=accumulator_model.predict(case,0,8)
                        image=build(tempo,state,first_duration=96,completed_frames=124,
                            request_offset=schedule[0],first_request_offset=schedule[1],selection_spin=schedule[2])
                        stats=lambda:dict(address=253,nonzero=dict(count=1,last=20000))
                        prior=dict(request=1,keyon_half=10000,end_half=20000,
                            completion_half=15000 if state=='completed' else 0,
                            playback=[dict(address=253,nonzero=dict(count=1,last=14000))] if state=='completed' else [stats()],
                            idle=[stats()] if state=='completed' else [dict(address=253,nonzero=dict(count=0,last=0))])
                        runs.append(dict(model=model,tempo=tempo,state=state,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),request_half=20000,
                            directory_half=24096,root_half=24096+observed,root_wait_ms=observed/2048,
                            continuity=dict(windows=[prior,{},{}])))
            return dict(schema='gbb-sgb-vendor-timer-tempo-v1',qualification=False,timer_continuity=True,
                first_duration=96,completed_frames=124,request_offset=schedule[0],first_request_offset=schedule[1],
                selection_spin=schedule[2],program_sha256={n:'0'*64 for n in ('sgb1.program.rom','sgb2.program.rom')},runs=runs)
        baseline=corpus((0,0,0));holdout=corpus((2,2,1536))
        report=accumulator_model.compare(baseline,holdout)
        self.assertIs(report['qualification'],False);self.assertIs(report['accuracy_passed'],True)
        changed=copy.deepcopy(holdout)
        changed['runs'][0]['root_half']+=16384;changed['runs'][0]['root_wait_ms']+=8
        rejected=accumulator_model.compare(baseline,changed)
        self.assertEqual(rejected['parameters'],report['parameters'])
        self.assertEqual(rejected['baseline'],report['baseline'])
        self.assertIs(rejected['accuracy_passed'],False)
        invalid=[]
        for key,value in (('request_offset',True),('qualification',True),('timer_continuity',False)):
            bad=copy.deepcopy(baseline);bad[key]=value;invalid.append(bad)
        for key,value in (('fixture_sha256','0'*64),('tempo',True),('root_wait_ms',float('nan'))):
            bad=copy.deepcopy(baseline);bad['runs'][0][key]=value;invalid.append(bad)
        bad=copy.deepcopy(baseline);bad['runs'][1]=bad['runs'][0];invalid.append(bad)
        for bad in invalid:
            with self.assertRaises(ValueError): accumulator_model.compare(bad,holdout)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);training=root/'baseline.json';testing=root/'holdout.json'
            training.write_text(json.dumps(baseline));testing.write_text(json.dumps(holdout))
            argv=['check','--baseline',str(training),'--holdout',str(testing)]
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),contextlib.redirect_stdout(stdout): accumulator_model.main()
            self.assertEqual(json.loads(stdout.getvalue()),report)
            for contents in ('{"private_detail":1}','x'*1048577):
                testing.write_text(contents);stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: accumulator_model.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded accumulator experiment failed; input diagnostics suppressed\n')

    def test_timer_continuity_bounds_and_completion(self):
        empty=lambda:dict(count=0,first=0,last=0,min_gap=0,max_gap=0)
        registers=lambda:[dict(address=a,all=empty(),nonzero=empty()) for a in range(0xFD,0x100)]
        first=dict(request=1,keyon_half=100,completion_half=200,end_half=300,
                   playback=registers(),idle=registers())
        first['playback'][0]['all']=dict(count=3,first=110,last=190,min_gap=40,max_gap=40)
        first['playback'][0]['nonzero']=dict(count=1,first=150,last=150,min_gap=0,max_gap=0)
        first['idle'][0]['all']=dict(count=2,first=210,last=290,min_gap=80,max_gap=80)
        first['idle'][0]['nonzero']=dict(count=1,first=290,last=290,min_gap=0,max_gap=0)
        second=dict(request=2,keyon_half=400,completion_half=0,end_half=500,
                    playback=registers(),idle=registers())
        data=dict(requests=[50,300],events=[dict(register=0x4C,request=1,half=100),
            dict(register=0x5C,value=255,request=1,half=200),dict(register=0x4C,request=2,half=400)],
            timer_continuity=[first,second])
        result=timer_tempo.continuity(data)
        self.assertEqual(result['prior_timer0_nonzero_to_request_ms'],10/2048)
        # A read immediately before delivery can share the recorded request
        # timestamp. Ownership comes from the callback's delivered count.
        boundary=copy.deepcopy(data)
        for kind in ('all','nonzero'):
            boundary['timer_continuity'][0]['idle'][0][kind]=dict(count=1,first=300,last=300,min_gap=0,max_gap=0)
        self.assertEqual(timer_tempo.continuity(boundary)['prior_timer0_nonzero_to_request_ms'],0)
        crossing=copy.deepcopy(boundary)
        crossing['timer_continuity'][0]['idle'][0]['all']['last']=301
        with self.assertRaises(ValueError): timer_tempo.continuity(crossing)
        faults=[]
        for field,value in (('request',2),('completion_half',201),('end_half',301),('keyon_half',True)):
            bad=copy.deepcopy(data);bad['timer_continuity'][0][field]=value;faults.append(bad)
        for field,value in (('count',True),('count',1000001),('first',199),('last',300),('min_gap',79)):
            bad=copy.deepcopy(data);bad['timer_continuity'][0]['idle'][0]['all'][field]=value;faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_continuity'][1]['idle'][0]['all']=first['idle'][0]['all'];faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_continuity'][0]['idle'][0]['nonzero']['count']=3;faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_continuity'][0]['idle'][0]['nonzero']['first']=205;faults.append(bad)
        bad=copy.deepcopy(data);bad['timer_continuity'][0]['private_bytes']=[1];faults.append(bad)
        faults.append({**data,'timer_continuity':[]})
        for bad in faults:
            with self.assertRaises(ValueError): timer_tempo.continuity(bad)
        active=copy.deepcopy(data);active['events'].pop(1)
        active['timer_continuity'][0]['completion_half']=0
        active['timer_continuity'][0]['idle']=registers()
        self.assertEqual(timer_tempo.continuity(active)['prior_timer0_nonzero_to_request_ms'],150/2048)

    def test_matched_timer_tempo_controls_and_decomposition(self):
        for tempo in timer_tempo.TEMPO_POINTS:
            active=build(tempo,'active',first_duration=96,completed_frames=124)
            completed=build(tempo,'completed',first_duration=96,completed_frames=124)
            self.assertEqual(active[0x4000:],completed[0x4000:])
            self.assertNotEqual(active[:0x4000],completed[:0x4000])
        self.assertEqual(build(),build(completed_frames=100))
        for frames in (True,99,125):
            with self.assertRaises(ValueError): build(completed_frames=frames)
        with self.assertRaises(ValueError): build(32,profile='stop',completed_frames=124)
        access=dict(zero_count=100,nonzero_after_stage_ms=[1.5+2*i for i in range(8)])
        measured=dict(root_wait_ms=15.6337890625,timers=[None,dict(wait=[access])])
        report=timer_tempo.decomposition(measured)
        self.assertEqual(report['timer0_nonzero_reads'],8)
        self.assertEqual(report['min_nonzero_gap_ms'],2)
        self.assertEqual(report['after_last_nonzero_ms'],.1337890625)
        access['nonzero_after_stage_ms']=[]
        report=timer_tempo.decomposition(measured)
        self.assertIsNone(report['after_last_nonzero_ms']);self.assertEqual(report['timer0_nonzero_reads'],0)

    def test_timer_tempo_reference_filters_and_failure(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            state=game.stem.rsplit('-',1)[-1]
            self.assertEqual(game.read_bytes(),build(64,state,request_offset=2,first_request_offset=2,
                selection_spin=1536,first_duration=96,completed_frames=124))
            self.assertEqual(flags,dict(timer_continuity=True) if '--continuity' in sys.argv else dict(selection_timer_results=True));self.assertEqual(clocks,300000000)
            return dict(selection_timer_results=True,state=state,model=model)
        def observation(data,state):
            self.assertEqual(state,data['state'])
            return dict(root_wait_ms=24 if data['model']=='sgb' else 25,
                        timers=[None,dict(wait=[dict(zero_count=100,nonzero_after_stage_ms=[1,3])])])
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),'--tempo','64','--tempo','64',
                  '--state','active','--state','completed','--state','active','--request-offset','2',
                  '--first-request-offset','2','--selection-spin','1536']
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(timer_tempo,'capture',side_effect=capture) as run,patch.object(timer_tempo,'observation',side_effect=observation),contextlib.redirect_stdout(stdout):
                timer_tempo.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,4)
            self.assertIs(report['qualification'],False);self.assertEqual(len(report['groups']),2)
            self.assertEqual(report['completed_frames'],124);self.assertEqual(report['first_duration'],96)
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv+['--continuity']),patch.object(timer_tempo,'capture',side_effect=capture),patch.object(timer_tempo,'observation',side_effect=observation),patch.object(timer_tempo,'continuity',return_value={'windows':[]}) as guard,contextlib.redirect_stdout(stdout):
                timer_tempo.main()
            report=json.loads(stdout.getvalue());self.assertIs(report['timer_continuity'],True)
            self.assertEqual(guard.call_count,4)
            stdout=io.StringIO();stderr=io.StringIO()
            with patch.object(sys,'argv',argv+['--continuity']),patch.object(timer_tempo,'capture',side_effect=capture),patch.object(timer_tempo,'observation',side_effect=observation),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised: timer_tempo.main()
            self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
            self.assertEqual(stderr.getvalue(),'bounded timer-tempo experiment failed; child diagnostics suppressed\n')
            for fault in (ValueError('private detail'),None):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(timer_tempo,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: timer_tempo.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded timer-tempo experiment failed; child diagnostics suppressed\n')

    def test_timer_window_bounds_and_stage_ownership(self):
        empty=lambda address:dict(address=address,count=0,first=0,last=0,min_gap=0,max_gap=0)
        window=dict(request=1,directory_half=2048,root_half=6144,
                    wait=[empty(a) for a in range(0xFD,0x100)],setup=[empty(a) for a in range(0xFD,0x100)])
        window['wait'][0].update(count=3,first=2100,last=6100,min_gap=2000,max_gap=2000)
        window['setup'][1].update(count=2,first=6200,last=6600,min_gap=400,max_gap=400)
        data=dict(music=[1],events=[dict(register=0x4C,request=1,half=10000)],
                  selection_directory_reads=[dict(request=1,address=0x2B00,half=2048)],
                  selection_timer_reads=[window])
        selections=[dict(traversal_stages=dict(root=dict(after_previous_stage_ms=2)))]
        report=phase.timer_summary(data,selections)[0]
        self.assertEqual(report['wait'][0]['count'],3)
        self.assertEqual(report['wait'][0]['max_gap_ms'],2000/2048)
        self.assertIsNone(report['wait'][1]['first_after_stage_ms'])
        for field,value in (('request',True),('request',2),('root_half',6145),('directory_half',2047)):
            bad=copy.deepcopy(data);bad['selection_timer_reads'][0][field]=value
            with self.assertRaises(ValueError): phase.timer_summary(bad,selections)
        for field,value in (('address',0xFC),('count',True),('count',1000001),('first',2047),
                            ('last',6144),('min_gap',2001),('max_gap',1999),('max_gap',4001),
                            ('count',2),('private_value',1)):
            bad=copy.deepcopy(data);bad['selection_timer_reads'][0]['wait'][0][field]=value
            with self.assertRaises(ValueError): phase.timer_summary(bad,selections)
        bad=copy.deepcopy(data);bad['selection_timer_reads'][0]['wait'][1]['first']=2050
        with self.assertRaises(ValueError): phase.timer_summary(bad,selections)
        bad=copy.deepcopy(data);bad['selection_timer_reads']=[]
        with self.assertRaises(ValueError): phase.timer_summary(bad,selections)
        positive=copy.deepcopy(data);positive['selection_timer_results']=True
        for stage in ('wait','setup'):
            for access in positive['selection_timer_reads'][0][stage]: access['nonzero']=[]
        positive['selection_timer_reads'][0]['wait'][0]['nonzero']=[2100,6100]
        result=phase.timer_summary(positive,selections)[0]['wait'][0]
        self.assertEqual((result['zero_count'],result['nonzero_count']),(1,2))
        self.assertEqual(result['nonzero_after_stage_ms'],[52/2048,4052/2048])
        for pulses in ([True],[2099],[6101],[6100,2100],[2100,2100],[2100]*129):
            bad=copy.deepcopy(positive);bad['selection_timer_reads'][0]['wait'][0]['nonzero']=pulses
            with self.assertRaises(ValueError): phase.timer_summary(bad,selections)
        bad=copy.deepcopy(positive);bad['selection_timer_results']=1
        with self.assertRaises(ValueError): phase.timer_summary(bad,selections)

    def test_phase_duration_controls_and_constant_rejection(self):
        default=build(112,'completed',request_offset=2)
        self.assertEqual(default,build(112,'completed',request_offset=2,first_duration=48))
        for duration in (24,96):
            image=build(112,'completed',request_offset=2,first_duration=duration)
            differences=[i for i,(a,b) in enumerate(zip(default,image)) if a!=b]
            self.assertEqual([i for i in differences if i not in (0x14E,0x14F)],[0x406E])
            self.assertEqual(image[0x406E],duration)
            self.assertEqual(image[0x14E:0x150],((sum(image)-sum(image[0x14E:0x150]))&65535).to_bytes(2,'big'))
        for duration in (True,0,25,256):
            with self.assertRaises(ValueError): build(first_duration=duration)
        for first,spin in ((0,0),(2,1024),(4,2499)):
            image=build(112,'completed',request_offset=2,first_request_offset=first,selection_spin=spin)
            self.assertEqual(image[0x4000:],default[0x4000:])
            self.assertEqual(image[0x14E:0x150],((sum(image)-sum(image[0x14E:0x150]))&65535).to_bytes(2,'big'))
        self.assertEqual(default,build(112,'completed',request_offset=2,first_request_offset=0,selection_spin=0))
        for first,spin in ((True,0),(-1,0),(5,0),(0,True),(0,-1),(0,2500)):
            with self.assertRaises(ValueError): build(first_request_offset=first,selection_spin=spin)
        with self.assertRaises(ValueError): build(32,profile='rapid',selection_spin=1)
        report=phase.constant_bound([15.599609375,20.0029296875])
        self.assertIs(report['constant_within_2ms_possible'],False)
        self.assertIsNone(report['admissible_constant_ms'])
        self.assertEqual(phase.constant_bound([16,20])['admissible_constant_ms'],[18,18])
        for values in ([],[True],[float('nan')],[float('inf')],[-1],[101]):
            with self.assertRaises(ValueError): phase.constant_bound(values)

    def test_phase_reference_filters_and_sanitization(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            self.assertEqual(game.read_bytes(),build(112,'completed',request_offset=2,first_duration=96,
                                                     first_request_offset=2,selection_spin=1024))
            self.assertEqual(flags,dict(selection_timer_results=True) if '--timer-results' in sys.argv else
                             dict(selection_timers=True) if '--timers' in sys.argv else dict(selection_traversal=True))
            self.assertEqual(clocks,260000000)
            data=dict(root_wait_ms=16 if model=='sgb' else 21)
            if '--timers' in sys.argv or '--timer-results' in sys.argv: data['selection_timer_reads']=[]
            if '--timer-results' in sys.argv: data['selection_timer_results']=True
            return data
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),
                  '--duration','96','--duration','96','--request-offset','2','--request-offset','2',
                  '--first-request-offset','2','--first-request-offset','2',
                  '--selection-spin','1024','--selection-spin','1024']
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(phase,'capture',side_effect=capture) as run,patch.object(phase,'observation',side_effect=lambda data:data),contextlib.redirect_stdout(stdout):
                phase.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,2)
            self.assertIs(report['qualification'],False)
            self.assertIs(report['constant_bound']['constant_within_2ms_possible'],False)
            self.assertTrue(all(r['first_request_offset']==2 and r['selection_spin']==1024 for r in report['runs']))
            with patch.object(sys,'argv',argv+['--timers']),patch.object(phase,'capture',side_effect=capture) as run,patch.object(phase,'observation',side_effect=lambda data:data),contextlib.redirect_stdout(io.StringIO()):
                phase.main()
            self.assertEqual(run.call_count,2)
            with patch.object(sys,'argv',argv+['--timer-results']),patch.object(phase,'capture',side_effect=capture) as run,patch.object(phase,'observation',side_effect=lambda data:data),contextlib.redirect_stdout(io.StringIO()):
                phase.main()
            self.assertEqual(run.call_count,2)
            stdout=io.StringIO();stderr=io.StringIO()
            with patch.object(sys,'argv',argv+['--timer-results']),patch.object(phase,'capture',return_value=dict(selection_timer_reads=[])),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised: phase.main()
            self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
            self.assertEqual(stderr.getvalue(),'bounded selection-phase experiment failed; child diagnostics suppressed\n')
            stdout=io.StringIO();stderr=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(phase,'capture',side_effect=ValueError('private detail')),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised: phase.main()
            self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
            self.assertEqual(stderr.getvalue(),'bounded selection-phase experiment failed; child diagnostics suppressed\n')

    def test_phase_observation_keeps_completed_traversal_guards(self):
        data=dict(requests=[2048,20480],events=[dict(register=0x4C,half=4096),
                  dict(register=0x5C,value=255,half=8192)],
                  selection_directory_reads=[dict(request=1,address=0x2B01,half=2500),
                                             dict(request=2,address=0x2B03,half=22528)],
                  selection_traversal_reads=[dict(request=1,address=0x2B10,half=3000),
                                             dict(request=2,address=0x2B14,half=57344)])
        stages=[None,dict(traversal_stages=dict(root=dict(after_previous_stage_ms=17))),
                dict(traversal_stages=dict(root=dict(after_previous_stage_ms=27)))]
        with patch.object(phase,'summarize',return_value=stages) as guard:
            self.assertEqual(phase.observation(data),dict(first_request_half=2048,request_half=20480,
                first_root_half=3000,directory_half=22528,root_half=57344,
                request_gap_ms=9,prior_note_to_request_ms=8,
                prior_release_to_request_ms=6,root_wait_ms=17,control_root_wait_ms=27))
            guard.assert_called_once_with(data,'base','completed')
        with patch.object(phase,'summarize',side_effect=ValueError('incomplete playback')):
            with self.assertRaises(ValueError): phase.observation(data)
        active=copy.deepcopy(data);active['events']=active['events'][:1]
        with patch.object(phase,'summarize',return_value=stages) as guard:
            self.assertIsNone(phase.observation(active,'active')['prior_release_to_request_ms'])
            guard.assert_called_once_with(active,'base','active')

    def test_build_scope_and_export(self):
        self.assertEqual(hashlib.sha256(firmware()).hexdigest(),'0455c511726ec625afee474a90eba960f5eae2e156b4772d6f7b82b338d36ffd')
        candidate=firmware(selection_wait=True);self.assertEqual(candidate,firmware(selection_wait=True))
        self.assertNotEqual(candidate,firmware())
        self.assertEqual(len(candidate),262144)
        self.assertEqual(sum(candidate)&65535,int.from_bytes(candidate[0x7FDE:0x7FE0],'little'))
        with self.assertRaises(ValueError): firmware(selection_wait=1)
        for state in ('active','completed'):
            for tempo in TEMPOS:
                baseline=build(tempo,state,False);holdout=build(tempo,state)
                self.assertEqual(holdout,build(tempo,state));self.assertNotEqual(baseline,holdout)
                self.assertEqual(baseline[0x4000:0x6000],holdout[0x4000:0x6000])
        for args in ((True,'active'),(0,'active'),(32,'bad')):
            with self.assertRaises(ValueError): build(*args)
        with self.assertRaises(ValueError): build(45,profile='stop')
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'candidate.rom'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_vendor_music.py'),
                     '--selection-wait','--output',str(path)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(path.read_bytes(),candidate)
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(path.read_bytes(),candidate)

    def test_comparison_requires_accuracy_and_improvement(self):
        for tempo in TEMPOS:
            original=[48,WAITS[tempo],27];baseline=[.13,.13,.13];candidate=[.16,WAITS[tempo]+.2,27.2]
            result=compare(original,baseline,candidate,tempo)
            self.assertTrue(all(r['absolute_error_ms']<=2 for r in result))
            for bad in ([1,*candidate[1:]],[.16,0,27.2],[.16,WAITS[tempo]+3,27.2]):
                with self.assertRaises(ValueError): compare(original,baseline,bad,tempo)
        with self.assertRaises(ValueError): compare([48,32,27],[.13,31,26],[.16,32,27],32)
        with self.assertRaises(ValueError): compare([48,32,27],[.13]*3,[.16,32,27],99)

    def test_reference_filters_and_failure_sanitization(self):
        for offset in (-1,5,True):
            with self.assertRaises(ValueError): build(request_offset=offset)
        def capture(probe,program,game,model,clocks,inputs,**flags):
            self.assertEqual(flags,dict(selection_traversal=True));self.assertEqual(clocks,260000000)
            self.assertEqual(game.read_bytes(),build(64,'completed',request_offset=2))
            return ([.13,.13,.13] if program.name=='default.rom' else
                    [.16,24.2,27.2] if program.name=='paced.rom' else [48,24,27])
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned test stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),
                  '--tempo','64','--tempo','64','--state','completed','--state','completed','--request-offset','2']
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=capture) as run,patch.object(reference,'waits',side_effect=lambda data,state:data),contextlib.redirect_stdout(stdout):
                reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,6)
            self.assertEqual(report['request_offset'],2);self.assertIs(report['qualification'],False)
            self.assertIs(report['opt_in'],True);self.assertTrue(all(len(r['cases'])==1 for r in report['runs']))
            stdout=io.StringIO();stderr=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=ValueError('private detail')),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised: reference.main()
            self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
            self.assertEqual(stderr.getvalue(),'bounded selection-wait comparison failed; child diagnostics suppressed\n')

    def capture(self,root,model,tempo=45,state='active',profile='normal'):
        game=root/'game.gb';game.write_bytes(build(tempo,state,profile=profile))
        child=subprocess.run([str(TIMELINE),str(root/'paced.rom'),str(game),model,'200000000',
            str(root/'none.script'),'fixture','--selection-traversal'],capture_output=True,timeout=180)
        self.assertEqual(child.returncode,0,child.stderr);self.assertLessEqual(len(child.stdout),65536)
        return json.loads(child.stdout)

    def test_native_waits_and_unmeasured_tempo(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'paced.rom').write_bytes(firmware(selection_wait=True))
            (root/'none.script').write_text('GBB SGB input v1\n0 none\n')
            controls=[(model,tempo,state) for model in ('sgb','sgb2') for tempo in (*TEMPOS,99,112)
                      for state in (('active','completed') if tempo in TEMPOS else ('active',))]
            image=firmware(selection_wait=True)
            def run(control):
                model,tempo,state=control;case=root/('-'.join((model,str(tempo),state)));case.mkdir()
                (case/'paced.rom').write_bytes(image);(case/'none.script').write_text('GBB SGB input v1\n0 none\n')
                return control,waits(self.capture(case,model,tempo,state),state)
            with ThreadPoolExecutor(max_workers=4) as pool:
                for (model,tempo,state),observed in pool.map(run,controls):
                    with self.subTest(model=model,tempo=tempo,state=state):
                        self.assertLess(observed[0],.25)
                        if tempo in WAITS: self.assertLess(abs(observed[1]-WAITS[tempo]),.4)
                        else: self.assertLess(observed[1],.4)
                        self.assertLess(abs(observed[2]-27),.4)

    def test_native_duration_phase_controls(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'paced.rom').write_bytes(firmware(selection_wait=True))
            (root/'none.script').write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for duration in (24,96):
                    with self.subTest(model=model,duration=duration):
                        game=root/'game.gb';game.write_bytes(build(112,'completed',request_offset=2,first_duration=duration))
                        data=reference.capture(TIMELINE,root/'paced.rom',game,model,260000000,
                                               root/'none.script',selection_traversal=True)
                        report=phase.observation(data)
                        self.assertGreater(report['prior_release_to_request_ms'],0)
                        self.assertLess(report['root_wait_ms'],.4)
                        self.assertLess(abs(report['control_root_wait_ms']-27),.4)

    def test_native_arrival_phase_controls(self):
        if TIMELINE is None or PROBE is None: self.skipTest('both probes not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'paced.rom').write_bytes(firmware(selection_wait=True))
            (root/'none.script').write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for spin in (512,1536):
                    with self.subTest(model=model,spin=spin):
                        game=root/'game.gb';game.write_bytes(build(112,'completed',request_offset=2,
                            first_request_offset=2,selection_spin=spin))
                        report=phase.observation(reference.capture(TIMELINE,root/'paced.rom',game,model,
                            260000000,root/'none.script',selection_traversal=True))
                        self.assertLess(report['root_wait_ms'],.4)
                        self.assertLess(abs(report['control_root_wait_ms']-27),.4)
                        self.assertEqual(report['root_half']-report['directory_half'],report['root_wait_ms']*2048)
                native=self.replay(root,model,112,first_request_offset=2,selection_spin=1536)
                scalar=self.replay(root,model,112,'scalar',first_request_offset=2,selection_spin=1536)
                self.assertEqual(native['pcm_fnv64'],scalar['pcm_fnv64'])
                self.assertEqual((native['starts'],native['notes']),(3,3))

    def test_native_timer_observation_transparency(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'paced.rom').write_bytes(firmware(selection_wait=True))
            (root/'none.script').write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for spin in (0,1536):
                    with self.subTest(model=model,spin=spin):
                        game=root/'game.gb';game.write_bytes(build(112,'completed',request_offset=2,
                            first_request_offset=2,selection_spin=spin))
                        captures=[reference.capture(TIMELINE,root/'paced.rom',game,model,260000000,
                            root/'none.script',**flags) for flags in
                            (dict(selection_traversal=True),dict(selection_timers=True),dict(selection_timer_results=True),dict(timer_continuity=True))]
                        plain,observed,results,continuous=captures;report=phase.observation(results)
                        continuity=timer_tempo.continuity(continuous)
                        self.assertEqual(continuity['windows'][0]['completion_half']>0,True)
                        del continuous['timer_continuity']
                        self.assertEqual(results,continuous)
                        self.assertTrue(all(a['count']==0 for a in report['timers'][1]['wait']))
                        self.assertGreater(report['timers'][2]['wait'][1]['count'],0)
                        self.assertGreater(report['timers'][2]['wait'][1]['nonzero_count'],0)
                        for window in results['selection_timer_reads']:
                            for stage in ('wait','setup'):
                                for access in window[stage]: del access['nonzero']
                        del results['selection_timer_results']
                        self.assertEqual(observed,results)
                        del observed['selection_timer_reads']
                        self.assertEqual(plain,observed)

    def test_native_matched_timer_tempo_controls(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);image=firmware(selection_wait=True)
            controls=[(model,tempo,state) for model in ('sgb','sgb2')
                      for tempo in timer_tempo.TEMPO_POINTS for state in ('active','completed')]
            def run(control):
                model,tempo,state=control;case=root/f'{model}-{tempo}-{state}';case.mkdir()
                (case/'paced.rom').write_bytes(image);(case/'none.script').write_text('GBB SGB input v1\n0 none\n')
                game=case/'game.gb';game.write_bytes(build(tempo,state,first_duration=96,completed_frames=124,
                    request_offset=2,first_request_offset=2,selection_spin=1536))
                data=reference.capture(TIMELINE,case/'paced.rom',game,model,300000000,
                    case/'none.script',timer_continuity=True)
                continuity=timer_tempo.continuity(data)
                if bool(continuity['windows'][0]['completion_half'])!=(state=='completed'):
                    raise ValueError('continuity playback state changed')
                return control,phase.observation(data,state)
            with ThreadPoolExecutor(max_workers=4) as pool:
                for (model,tempo,state),measured in pool.map(run,controls):
                    with self.subTest(model=model,tempo=tempo,state=state):
                        self.assertEqual(measured['prior_release_to_request_ms'] is None,state=='active')
                        self.assertLess(abs(measured['control_root_wait_ms']-27),.4)
                        access=measured['timers'][1]['wait'][1]
                        if tempo in WAITS:
                            self.assertLess(abs(measured['root_wait_ms']-WAITS[tempo]),.4)
                            self.assertGreater(access['nonzero_count'],0)
                        else:
                            self.assertLess(measured['root_wait_ms'],.4)
                            self.assertEqual(access['nonzero_count'],0)

    def replay(self,root,model,tempo,mode='native',profile='normal',*,first_request_offset=0,selection_spin=0):
        game=root/'game.gb';game.write_bytes(build(tempo,profile=profile,
            first_request_offset=first_request_offset,selection_spin=selection_spin))
        child=subprocess.run([str(PROBE),str(root/'paced.rom'),str(game),model,'100000000',mode,'fixture'],
            capture_output=True,timeout=180)
        self.assertEqual(child.returncode,0,child.stderr);d=json.loads(child.stdout)
        self.assertIs(d['reset_equal'],True);self.assertIs(d['restore_equal'],True)
        self.assertGreater(d['selection_wait_restores'],0);self.assertGreater(d['selection_wait_pulse_restores'],0);self.assertGreater(d['unread_restores'],0)
        self.assertGreater(d['nonzero'],100);self.assertEqual(d['rejected'],0)
        return d

    def test_wait_snapshot_scalar_and_cancellation(self):
        if PROBE is None or TIMELINE is None: self.skipTest('both probes not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'paced.rom').write_bytes(firmware(selection_wait=True))
            (root/'none.script').write_text('GBB SGB input v1\n0 none\n')
            if INTERRUPT is not None:
                game=root/'reload.gb';game.write_bytes(build(32))
                child=subprocess.run([str(INTERRUPT),str(root/'paced.rom'),str(game)],capture_output=True,timeout=10)
                self.assertEqual(child.returncode,0,child.stderr)
                report=json.loads(child.stdout);self.assertIs(report['mid_wait_reload'],True)
                self.assertIs(report['stale_keyon'],False);self.assertLess(report['cycles'],1024)
            for model in ('sgb','sgb2'):
                for tempo in (32,128):
                    native=self.replay(root,model,tempo);scalar=self.replay(root,model,tempo,'scalar')
                    self.assertEqual(native['pcm_fnv64'],scalar['pcm_fnv64'])
                    self.assertEqual((native['starts'],native['completes'],native['notes']),(3,1,3))
                for profile in ('rapid','stop','upload'):
                    with self.subTest(model=model,profile=profile):
                        data=self.capture(root,model,32,profile=profile)
                        notes=[e for e in data['events'] if e['register']==0x4C]
                        self.assertEqual([e['pitch'] for e in notes],[1437,1357,1281] if profile=='upload' else [1437,1281])
                        self.assertEqual([e['request'] for e in notes],[1,2,3] if profile=='upload' else [1,3 if profile=='rapid' else 4])
                        d=self.replay(root,model,32,profile=profile)
                        self.assertEqual((d['starts'],d['completes'],d['notes'],d['selected']),(3,1,3 if profile=='upload' else 2,3))
                        final_request=notes[-1]['request']
                        addresses=(0x2B04,0x2B05)
                        directory=max(r['half'] for r in data['selection_directory_reads']
                            if r['request']==final_request and r['address'] in addresses)
                        root_read=min(r['half'] for r in data['selection_traversal_reads']
                            if r['request']==final_request and r['address']==0x2B18 and r['half']>=directory)
                        interval=(root_read-directory)/2048
                        self.assertLess(abs(interval-32),.4) if profile=='rapid' else self.assertLess(interval,.25)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path);parser.add_argument('--probe',type=Path);parser.add_argument('--interrupt-probe',type=Path)
    args,rest=parser.parse_known_args();TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    INTERRUPT=args.interrupt_probe.resolve() if args.interrupt_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
