#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned dispatch fixtures, readback observation guards and exact replay."""
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
from build_sgb_vendor_dispatch_fixture import build, PROFILES, STATES
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_dispatch_reference import summarize

PROBE = TIMELINE = None
REPLIES = False


def synthetic():
    notes = [dict(register=0x4C,value=4,half=t+900,request=i+1,
                  pitch=pitch,srcn=2,adsr1=255,adsr2=224,gain=184)
             for i,(t,pitch) in enumerate(zip((100,10000,20000),(1437,1357,1281)))]
    records = [dict(request=i+1,port=port,half=t,first_read=t+10,last_read=t+20,
                    first_equal=t+20,last_equal=t+20,equal_reads=1,unequal_reads=1)
               for i,t in enumerate((110,10010,20010)) for port in (0,3)]
    return dict(music=[1,2,3],requests=[100,10000,20000],events=notes,
                final=dict(flg=0,kof=0,echo_left=0,echo_right=0),port_handshakes=records)


class DispatchContracts(unittest.TestCase):
    def test_fixtures_and_export(self):
        images = [build(p,s) for p in PROFILES for s in STATES]
        self.assertEqual(len({hashlib.sha256(i).digest() for i in images}),4)
        for profile in PROFILES:
            for state in STATES:
                self.assertEqual(build(profile,state),build(profile,state))
                self.assertEqual(len(build(profile,state)),32768)
        for profile,state in (('bad','active'),('change','bad')):
            with self.assertRaises(ValueError): build(profile,state)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'game.gb'
            command = [sys.executable,str(Path(__file__).resolve().parents[1]/
                       'scripts/build_sgb_vendor_dispatch_fixture.py'),'--output',str(output)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(output.read_bytes(),build())
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(output.read_bytes(),build())

    def test_summary_and_guards(self):
        data = synthetic()
        result = summarize(data,'change','active')
        self.assertEqual([s['music'] for s in result],[1,2,3])
        self.assertEqual(result[0]['writes'][0]['first_equal_after_write_ms'],20/2048)
        no_read = copy.deepcopy(data)
        for record in no_read['port_handshakes']:
            for key in ('first_read','last_read','first_equal','last_equal','equal_reads','unequal_reads'):
                record[key] = 0
        self.assertIsNone(summarize(no_read,'change','active')[0]['writes'][0]['first_equal_after_write_ms'])
        for fault in ('missing','unknown','type','order','request','port','early','late',
                      'read-early','read-late','equal-early','equal-late','no-reads',
                      'no-equals','read-bound','note','state','final'):
            with self.subTest(fault=fault):
                changed = copy.deepcopy(data)
                record = changed['port_handshakes'][0]
                if fault == 'missing': changed.pop('port_handshakes')
                elif fault == 'unknown': record['value'] = 1
                elif fault == 'type': record['half'] = True
                elif fault == 'order': changed['port_handshakes'].reverse()
                elif fault == 'request': record['request'] = 4
                elif fault == 'port': record['port'] = 4
                elif fault == 'early': record['half'] = 99
                elif fault == 'late': record['half'] = 1000
                elif fault == 'read-early': record['first_read'] = 109
                elif fault == 'read-late': record['last_read'] = 1001
                elif fault == 'equal-early': record['first_equal'] = 119
                elif fault == 'equal-late': record['last_equal'] = 131
                elif fault == 'no-reads': record['equal_reads'] = record['unequal_reads'] = 0
                elif fault == 'no-equals': record['equal_reads'] = 0
                elif fault == 'read-bound': record['unequal_reads'] = 1000000
                elif fault == 'note': changed['events'][1]['pitch'] = 1437
                elif fault == 'state': changed['events'].insert(1,dict(register=0x5C,value=255,half=2000))
                elif fault == 'final': changed['final']['flg'] = 128
                with self.assertRaises(ValueError): summarize(changed,'change','active')
        replaced = copy.deepcopy(data)
        replaced['port_handshakes'].insert(1,dict(replaced['port_handshakes'][0],half=125,
            first_read=0,last_read=0,first_equal=0,last_equal=0,equal_reads=0,unequal_reads=0))
        with self.assertRaises(ValueError): summarize(replaced,'change','active')

    def test_physical_dispatch_and_replay(self):
        if PROBE is None or TIMELINE is None: self.skipTest('playback probes not supplied')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); rom = root/'owned.rom'; rom.write_bytes(firmware())
            inputs = root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for profile in PROFILES:
                    for state in STATES:
                        with self.subTest(model=model,profile=profile,state=state):
                            game = root/'game.gb'; game.write_bytes(build(profile,state))
                            command = [str(TIMELINE),str(rom),str(game),model,'200000000',str(inputs),'fixture']
                            child = subprocess.run(command+['--spc-replies' if REPLIES else '--port-handshakes'],capture_output=True,timeout=120)
                            self.assertEqual(child.returncode,0,child.stderr)
                            data = json.loads(child.stdout)
                            if REPLIES:
                                from check_sgb_vendor_reply_reference import summarize as reply_summary
                                self.assertEqual(len(reply_summary(data,profile,state)),3)
                                data.pop('spc_reply_handshakes')
                            else:
                                self.assertEqual(len(summarize(data,profile,state)),3)
                            baseline = subprocess.run(command,capture_output=True,timeout=120)
                            self.assertEqual(baseline.returncode,0,baseline.stderr)
                            data.pop('port_handshakes')
                            self.assertEqual(data,json.loads(baseline.stdout))
                            if profile == 'change':
                                child = subprocess.run([str(PROBE),str(rom),str(game),model,
                                    '200000000','native','fixture'],capture_output=True,timeout=180)
                                self.assertEqual(child.returncode,0,child.stderr)
                                report = json.loads(child.stdout)
                                self.assertIs(report['reset_equal'],True)
                                self.assertIs(report['restore_equal'],True)
                                self.assertEqual((report['starts'],report['completes'],report['notes'],report['rejected']),
                                                 (3,1 if state == 'active' else 3,3,0))
                                self.assertGreater(report['nonzero'],100)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--timeline-probe',type=Path)
    args,rest = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    TIMELINE = args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
