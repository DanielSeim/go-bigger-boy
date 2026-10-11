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

TIMELINE=PROBE=INTERRUPT=None


class SelectionWaitContracts(unittest.TestCase):
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
            self.assertEqual(flags,dict(selection_timer_results=True));self.assertEqual(clocks,300000000)
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
                            (dict(selection_traversal=True),dict(selection_timers=True),dict(selection_timer_results=True))]
                        plain,observed,results=captures;report=phase.observation(results)
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
                    case/'none.script',selection_timer_results=True)
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
