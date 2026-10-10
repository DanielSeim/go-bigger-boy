#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public physical playback and bounded-report contracts for owned phase fixtures."""
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
from build_sgb_vendor_phase_fixture import build, sequence, CASES
from check_sgb_vendor_phase_reference import summarize, compare

PROBE = TIMELINE = None


class PhaseContracts(unittest.TestCase):
    def test_fixture_and_report_contract(self):
        images = [build(case) for case in CASES]
        self.assertEqual(len({hashlib.sha256(image).hexdigest() for image in images}),len(CASES))
        for case,image in zip(CASES,images):
            self.assertEqual(image,build(case))
            self.assertEqual(len(image),32768)
        with self.assertRaises(ValueError): build('unknown')
        # A minimal delivered single-selection report exercises ownership and
        # cumulative phase independently of execution timing tolerances.
        data = dict(music=[1]*9,requests=list(range(0,9000,1000)),
                    events=[dict(register=0x4C,value=4,half=500+i*1000,request=i+1,pitch=1437) for i in range(9)],
                    final=dict(flg=0,kof=0,echo_left=0,echo_right=0))
        report = summarize(data,'selection')
        self.assertEqual(report['selection_ms'],[500/2048]*9)
        self.assertEqual(compare(report,report)['selection_error_ms'],[0]*9)
        for field in ('events','music','final'):
            changed = copy.deepcopy(data)
            if field == 'events': changed[field][0]['request'] = 2
            elif field == 'music': changed[field][0] = 128
            else: changed[field]['echo_left'] = 1
            with self.assertRaises(ValueError): summarize(changed,'selection')
        changed = dict(report,pitches=[0])
        with self.assertRaises(ValueError): compare(report,changed)
        events = []
        for i in range(8):
            start = 1000+i*2200
            events.extend((dict(register=0x4C,value=4,half=start,request=1,pitch=1437),
                           dict(register=0x5C,value=4,half=start+1500),
                           dict(register=0x5C,value=0,half=start+1510)))
        events.extend((dict(register=0x5C,value=255,half=start+2000),
                       dict(register=0x5C,value=0,half=start+2010)))
        chain = summarize(dict(data,music=[1],requests=[0],events=events),'long-chain')
        self.assertEqual(chain['phase_ms'],[i*2200/2048 for i in range(8)])
        changed = copy.deepcopy(chain); changed['phase_ms'][-1] += 8
        self.assertEqual(compare(chain,changed)['phase_error_ms'],[0]*7+[8])
        # CLI export is reproducible and must never overwrite an existing file.
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'owned.gb'
            command = [sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_vendor_phase_fixture.py'),'--output',str(output)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            original = output.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(output.read_bytes(),original)

    def test_physical_chains_and_selections(self):
        if TIMELINE is None or PROBE is None: self.skipTest('playback probes not supplied')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root/'owned.rom'; rom.write_bytes(firmware())
            inputs = root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for case in CASES:
                    with self.subTest(model=model,case=case):
                        game = root/'game.gb'; game.write_bytes(build(case))
                        child = subprocess.run([str(TIMELINE),str(rom),str(game),model,'300000000',str(inputs),'fixture'],capture_output=True,timeout=120)
                        self.assertEqual(child.returncode,0,child.stderr)
                        measured = summarize(json.loads(child.stdout),case)
                        self.assertTrue(all(value > 0 for value in measured['selection_ms']))
                        if 'phase_ms' in measured:
                            self.assertEqual(measured['phase_ms'][0],0)
                            self.assertEqual(measured['phase_ms'],sorted(set(measured['phase_ms'])))
                            self.assertGreater(measured['timing']['completion_ms'],0)
                game.write_bytes(build('long-rest-chain'))
                child = subprocess.run([str(PROBE),str(rom),str(game),model,'300000000','native','fixture'],capture_output=True,timeout=240)
                self.assertEqual(child.returncode,0,child.stderr)
                report = json.loads(child.stdout)
                self.assertIs(report['reset_equal'],True); self.assertIs(report['restore_equal'],True)
                self.assertEqual((report['starts'],report['completes'],report['notes'],report['flg'],report['rejected']),(1,1,8,0,0))
                self.assertGreater(report['unread_restores'],0)
                self.assertGreater(report['nonzero'],100)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--timeline-probe',type=Path)
    args,rest = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    TIMELINE = args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
