#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in root pacing, direct error gates and interruptible snapshot continuation."""
import argparse
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

TIMELINE=PROBE=INTERRUPT=None


class SelectionWaitContracts(unittest.TestCase):
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

    def replay(self,root,model,tempo,mode='native',profile='normal'):
        game=root/'game.gb';game.write_bytes(build(tempo,profile=profile))
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
