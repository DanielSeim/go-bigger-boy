#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measured voice-volume points, held-out setup, fallback and exact lifecycle."""
import argparse
import copy
import contextlib
import io
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_vendor_volume_fixture import build,CASES
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_volume_reference import check,summarize,EXPECTED
import check_sgb_vendor_volume_reference as reference

TIMELINE=PROBE=None


def synthetic():
    return dict(music=[1,2,3],requests=[100,200,300],
        events=[dict(request=i,half=i*100+50,register=0x4C,value=4,
            pitch=717,srcn=2,adsr1=255,adsr2=224,gain=184) for i in (1,2,3)],
        selection_volume_writes=[dict(request=i,half=i*100+20+j,register=r,value=v)
            for i,values in enumerate(EXPECTED['pan'],1) for j,(r,v) in enumerate(zip((32,33),values))],
        final=dict(flg=0,kof=0,echo_left=0,echo_right=0))


class VolumeContracts(unittest.TestCase):
    def test_build_and_export(self):
        images=[build(c,h) for c in CASES for h in (False,True)]
        self.assertEqual(len(set(images)),6)
        for image in images:
            self.assertEqual(len(image),32768)
            self.assertEqual(int.from_bytes(image[0x14E:0x150],'big'),sum(image[:0x14E]+image[0x150:])&65535)
        self.assertEqual(images,[build(c,h) for c in CASES for h in (False,True)])
        for c,h in (('bad',False),('pan',1)):
            with self.assertRaises(ValueError): build(c,h)
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)/'game.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/
                'scripts/build_sgb_vendor_volume_fixture.py'),'--case','velocity','--holdout','--output',str(out)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(out.read_bytes(),build('velocity',True))
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(out.read_bytes(),build('velocity',True))

    def test_report_guards(self):
        data=synthetic()
        self.assertEqual([s['voice'] for s in check(data,'pan')],EXPECTED['pan'])
        for fault in ('side','value','request','time','register','bool','count','pitch','source','final'):
            bad=copy.deepcopy(data)
            if fault=='side': bad['selection_volume_writes'].pop()
            elif fault=='value': bad['selection_volume_writes'][0]['value']=8
            elif fault=='request': bad['selection_volume_writes'][0]['request']=0
            elif fault=='time': bad['selection_volume_writes'][0]['half']=150
            elif fault=='register': bad['selection_volume_writes'][0]['register']=2
            elif fault=='bool': bad['selection_volume_writes'][0]['value']=True
            elif fault=='count': bad['selection_volume_writes']*=86
            elif fault=='pitch': bad['events'][0]['pitch']=100
            elif fault=='source': bad['events'][0]['srcn']=1
            else: bad['final']['kof']=4
            with self.subTest(fault=fault),self.assertRaises(ValueError): check(bad,'pan')

    def test_reference_gates_and_suppresses_failures(self):
        identities={hashlib.sha256(build(c,h)).digest():(c,h) for c in CASES for h in (False,True)}
        def capture(probe,program,game,model,clocks,inputs,**flags):
            self.assertEqual(flags,dict(selection_volume=True));self.assertEqual(clocks,200000000)
            case,holdout=identities[hashlib.sha256(game.read_bytes()).digest()]
            data=synthetic()
            for n,pitch in zip(data['events'],[1437,1357,1281] if holdout else [717]*3): n['pitch']=pitch
            for i,pair in enumerate(EXPECTED[case]):
                for j,value in enumerate(pair): data['selection_volume_writes'][i*2+j]['value']=value
            return data
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('sgb1.program.rom','sgb2.program.rom'): (root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root)]
            stdout=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(stdout):
                reference.main()
            report=json.loads(stdout.getvalue());self.assertEqual(run.call_count,24)
            self.assertEqual(report['schema'],'gbb-sgb-vendor-volume-v1')
            self.assertIs(report['qualification'],False)
            self.assertTrue(all(c['voice_equal'] for r in report['runs'] for c in r['cases']))
            for failure in (ValueError('private child detail'),subprocess.CalledProcessError(2,'private child')):
                stdout=io.StringIO();stderr=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=failure),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised: reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(stdout.getvalue(),'')
                self.assertEqual(stderr.getvalue(),'bounded voice-volume reference comparison failed\n')

    def timeline(self,rom,game,model,flag=True):
        command=[str(TIMELINE),str(rom),str(game),model,'200000000',str(game.parent/'none.script'),'fixture']
        if flag: command.append('--selection-volume')
        child=subprocess.run(command,capture_output=True,timeout=180)
        self.assertEqual(child.returncode,0,child.stderr)
        self.assertLessEqual(len(child.stdout),98304)
        return json.loads(child.stdout)

    def test_native_volume_and_observer(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);rom=root/'host.rom';rom.write_bytes(firmware());game=root/'game.gb'
            (root/'none.script').write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for case in CASES:
                    for holdout in (False,True):
                        with self.subTest(model=model,case=case,holdout=holdout):
                            game.write_bytes(build(case,holdout));observed=self.timeline(rom,game,model)
                            self.assertEqual([s['voice'] for s in check(observed,case,holdout)],EXPECTED[case])
                            if case=='velocity' and holdout:
                                for key in ('selection_volume_writes','selection_traversal_reads','selection_directory_reads',
                                    'handoff_writes','selection_pitch_writes','spc_reply_handshakes','port_handshakes'):
                                    observed.pop(key)
                                self.assertEqual(observed,self.timeline(rom,game,model,False))
            # A single unmeasured operand must retain the provisional renderer.
            # Song volume 161 at centered pan and velocity 15 evaluates to 19/19.
            image=bytearray(build('pan'))
            for i in range(3): image[0x4069+48*i]=161;image[0x4067+48*i]=10
            image[0x14E:0x150]=bytes(2)
            image[0x14E:0x150]=(sum(image)&65535).to_bytes(2,'big')
            game.write_bytes(image)
            result=summarize(self.timeline(rom,game,'sgb2'),'pan')
            self.assertEqual([s['voice'] for s in result],[[19,19]]*3)

    def test_replay_and_scalar(self):
        if PROBE is None: self.skipTest('replay probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);rom=root/'host.rom';rom.write_bytes(firmware());game=root/'game.gb'
            for model in ('sgb','sgb2'):
                for case in ('pan','volume','velocity'):
                    with self.subTest(model=model,case=case):
                        game.write_bytes(build(case,True));reports=[]
                        for mode in ('native','scalar'):
                            child=subprocess.run([str(PROBE),str(rom),str(game),model,'100000000',mode,'fixture'],
                                capture_output=True,timeout=180)
                            self.assertEqual(child.returncode,0,child.stderr);d=json.loads(child.stdout);reports.append(d)
                            self.assertIs(d['reset_equal'],True);self.assertIs(d['restore_equal'],True)
                            self.assertGreater(d['restores'],0);self.assertGreater(d['unread_restores'],0)
                            self.assertEqual((d['starts'],d['completes'],d['notes'],d['rejected']),(3,3,3,0))
                            self.assertGreater(d['nonzero'],100)
                        self.assertEqual(reports[0]['pcm_fnv64'],reports[1]['pcm_fnv64'])
                        self.assertEqual(reports[0]['frames'],reports[1]['frames'])


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path);parser.add_argument('--probe',type=Path)
    args,rest=parser.parse_known_args()
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
