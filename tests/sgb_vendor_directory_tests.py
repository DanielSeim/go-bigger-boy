#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded selected-directory observations and native playback controls."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_vendor_handoff_tests as handoff
from build_sgb_vendor_handoff_fixture import build, PROFILES, STATES
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_directory_reference import summarize

TIMELINE = None


def synthetic():
    data=handoff.synthetic()
    data['selection_directory_reads']=[dict(request=e['request'],half=e['half']-20+(1-i),
        address=0x2B00+2*(data['music'][e['request']-1]-1)+i)
        for e in data['events'] if e['register']==0x4C for i in (1,0)]
    # Original lookup may read high then low; both are accepted in time order.
    data['selection_directory_reads'].sort(key=lambda r:r['half'])
    return data


class DirectoryContracts(unittest.TestCase):
    def test_summary_and_guards(self):
        data=synthetic(); result=summarize(data,'base','active')
        self.assertEqual(len(result),3)
        self.assertEqual(result[0]['first_directory_read_ms'],880/2048)
        self.assertEqual(result[0]['pitch_setup_after_last_directory_read_ms'],9/2048)
        extra=copy.deepcopy(data)
        extra['selection_directory_reads'].insert(0,dict(request=1,half=120,address=0x2B04))
        self.assertEqual(len(summarize(extra,'base','active')[0]['directory_reads']),3)
        # Validation and playback can revisit a word: preserve both observations.
        repeated=copy.deepcopy(data)
        repeated['selection_directory_reads'].insert(1,dict(repeated['selection_directory_reads'][0]))
        self.assertEqual(len(summarize(repeated,'base','active')[0]['directory_reads']),3)
        for fault in ('missing','empty','bound','extra','bool','request','address','early','late','order','missing-byte','wrong-word'):
            with self.subTest(fault=fault):
                changed=copy.deepcopy(data); reads=changed['selection_directory_reads']; r=reads[0]
                if fault=='missing': changed.pop('selection_directory_reads')
                elif fault=='empty': reads.clear()
                elif fault=='bound': reads*=65
                elif fault=='extra': r['value']=0
                elif fault=='bool': r['half']=True
                elif fault=='request': r['request']=0
                elif fault=='address': r['address']=0x2B06
                elif fault=='early': r['half']=99
                elif fault=='late': r['half']=1000
                elif fault=='order': reads.reverse()
                elif fault=='missing-byte': reads.pop(0)
                elif fault=='wrong-word': r['address']=0x2B04
                with self.assertRaises(ValueError): summarize(changed,'base','active')

    def test_physical_directory(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); rom=root/'owned.rom'; rom.write_bytes(firmware())
            inputs=root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for profile in PROFILES:
                    for state in STATES:
                        with self.subTest(model=model,profile=profile,state=state):
                            game=root/'game.gb'; game.write_bytes(build(profile,state))
                            command=[str(TIMELINE),str(rom),str(game),model,'200000000',str(inputs),'fixture']
                            child=subprocess.run(command+['--selection-directory'],capture_output=True,timeout=120)
                            self.assertEqual(child.returncode,0,child.stderr)
                            self.assertLessEqual(len(child.stdout),49152)
                            data=json.loads(child.stdout)
                            self.assertEqual(len(summarize(data,profile,state)),3)
                            for key in ('selection_directory_reads','handoff_writes','selection_pitch_writes',
                                        'spc_reply_handshakes','port_handshakes'):
                                data.pop(key)
                            baseline=subprocess.run(command,capture_output=True,timeout=120)
                            self.assertEqual(baseline.returncode,0,baseline.stderr)
                            self.assertEqual(data,json.loads(baseline.stdout))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--timeline-probe',type=Path)
    args,rest=parser.parse_known_args()
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
