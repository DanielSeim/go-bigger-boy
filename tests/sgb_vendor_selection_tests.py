#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned selection-isolation transport, port-window and lifecycle contracts."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_selection_fixture import build, PROFILES, STATES
from check_sgb_vendor_selection_reference import summarize

PROBE=TIMELINE=None


class SelectionContracts(unittest.TestCase):
    def test_fixture_and_port_summary(self):
        images=[build(p,s) for p in PROFILES for s in STATES]
        self.assertEqual(len({hashlib.sha256(i).digest() for i in images}),16)
        for (p,s),image in zip(((p,s) for p in PROFILES for s in STATES),images):
            self.assertEqual(image,build(p,s)); self.assertEqual(len(image),32768)
        for p,s in (('bad','active'),('base','bad')):
            with self.assertRaises(ValueError): build(p,s)
        data=dict(music=[1,1],requests=[100,10000],final=dict(flg=0,kof=0,echo_left=0,echo_right=0),
                  events=[dict(register=0x4C,value=4,half=t,request=i+1,pitch=1437,srcn=2,adsr1=255,adsr2=224,gain=184) for i,t in enumerate((1000,11000))],
                  port_windows=[dict(request=i+1,first=t+100,last=t+200,keyon=k,writes=[1,2,1,1]) for i,(t,k) in enumerate(((100,1000),(10000,11000)))])
        result=summarize(data,'active')
        self.assertEqual(result['selections'][0]['selection_ms'],900/2048)
        self.assertEqual(result['selections'][1]['last_port_ms'],200/2048)
        self.assertEqual(result['selections'][1]['keyon_after_last_port_ms'],800/2048)
        for fault in ('missing','late','count','ownership','note-ownership','state'):
            changed=copy.deepcopy(data)
            if fault=='missing': del changed['port_windows']
            elif fault=='late': changed['port_windows'][0]['last']=1000
            elif fault=='count': changed['port_windows'][0]['writes'][0]=-1
            elif fault=='ownership': changed['port_windows'][1]['request']=1
            elif fault=='note-ownership': changed['events'][0]['request']=2
            else: changed['events'].insert(1,dict(register=0x5C,value=255,half=2000))
            with self.assertRaises(ValueError): summarize(changed,'active')
        complete=copy.deepcopy(data); complete['events'].insert(1,dict(register=0x5C,value=255,half=2000))
        self.assertIs(summarize(complete,'completed')['first_completed'],True)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'owned.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_vendor_selection_fixture.py'),'--output',str(path)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            before=path.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(before,path.read_bytes())

    def test_physical_isolation(self):
        if TIMELINE is None or PROBE is None: self.skipTest('playback probes not supplied')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); rom=root/'owned.rom'; rom.write_bytes(firmware())
            inputs=root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for profile in PROFILES:
                    for state in STATES:
                        with self.subTest(model=model,profile=profile,state=state):
                            game=root/'game.gb'; game.write_bytes(build(profile,state))
                            child=subprocess.run([str(TIMELINE),str(rom),str(game),model,'180000000',str(inputs),'fixture'],capture_output=True,timeout=120)
                            self.assertEqual(child.returncode,0,child.stderr)
                            report=summarize(json.loads(child.stdout),state)
                            for item in report['selections']:
                                self.assertGreater(item['keyon_after_last_port_ms'],0)
                for profile,state in (('prefix64','active'),('tail64','completed')):
                    game.write_bytes(build(profile,state))
                    child=subprocess.run([str(PROBE),str(rom),str(game),model,'180000000','native','fixture'],capture_output=True,timeout=180)
                    self.assertEqual(child.returncode,0,child.stderr)
                    report=json.loads(child.stdout)
                    self.assertIs(report['reset_equal'],True); self.assertIs(report['restore_equal'],True)
                    self.assertEqual((report['starts'],report['completes'],report['notes'],report['rejected'],report['flg']),
                                     (2,1 if state=='active' else 2,2,0,0))
                    self.assertGreater(report['nonzero'],100)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--timeline-probe',type=Path)
    args,rest=parser.parse_known_args()
    PROBE=args.probe.resolve() if args.probe else None
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
