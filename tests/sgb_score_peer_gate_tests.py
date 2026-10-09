#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DA bounded peer gate calibration with frozen D9 controls."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_score_async_adsr_tests as previous
from sgb_score_initial_tick_tests import InitialTickTests, IMAGE_HASH as D9_HASH

IMAGE_HASH='1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82'


class PeerGateTests(previous.AsyncAdsrTests):
    VERSION=0xDA
    BUILD_OPTIONS={**InitialTickTests.BUILD_OPTIONS,'peer_gate_timing':True}
    EXPECTED_IMAGE_HASH=IMAGE_HASH
    RELEASE_PHASE_PROFILES=previous.AsyncAdsrTests.RELEASE_PHASE_PROFILES|{(10,'rests')}

    def trajectories(self,result,case,held_voice,slots):
        super().trajectories(result,case,held_voice,slots)
        timing=previous.reference_timing(result['envelope_notes'],case,held_voice)
        self.assertTrue(all(note['gate_matches'] and note['onset_matches'] for note in timing),timing)

    def test_da_build_dependencies_caps_and_frozen_d9(self):
        image=previous.program(**self.BUILD_OPTIONS)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),IMAGE_HASH)
        self.assertEqual(hashlib.sha256(previous.program(**InitialTickTests.BUILD_OPTIONS)).hexdigest(),D9_HASH)
        for flags in ({'peer_gate_timing':True},{**self.BUILD_OPTIONS,'peer_gate_timing':1},
                      {**self.BUILD_OPTIONS,'initial_score_tick':False}):
            with self.assertRaises(ValueError):
                previous.program(**flags)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'da.rom'
            command=[sys.executable,str(previous.ROOT/'scripts/build_sgb_score_transport.py'),
                     *['--'+key.replace('_','-') for key in self.BUILD_OPTIONS],'--output',str(path)]
            for expected in (0,2):
                child=subprocess.run(command,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,expected,child.stderr)
                self.assertEqual(path.read_bytes(),image)

    def test_frozen_d9_late_peer_gate_miss_remains_visible(self):
        result=InitialTickTests().probe('sgb','retrigger',2)
        timing=previous.reference_timing(result['envelope_notes'],'retrigger',2)
        self.assertLess(timing[-1]['gate_spc_cycles'],72700)
        self.assertTrue(timing[-1]['gate_matches'])  # The wider window hid the exact miss.


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining=parser.parse_known_args()
    previous.PROBE=args.probe.resolve() if args.probe else None
    suite=unittest.main(argv=[sys.argv[0],*remaining],exit=False)
    if previous.EVIDENCE:
        da=[row for row in previous.EVIDENCE if row['version']==0xDA]
        print(json.dumps({'schema':'gbb-sgb-score-peer-gate-v1','qualification':False,'playback':False,
            'image_sha256':IMAGE_HASH,
            'suite_passed':suite.result.wasSuccessful(),'reference_timing_allowance_spc_cycles':4096,
            'timing_qualified':bool(da) and all(note['gate_matches'] and note['onset_matches'] for row in da for note in row['timing']),
            'runs':da,'d9_control_runs':[row for row in previous.EVIDENCE if row['version']==0xD9]},indent=2))
    sys.exit(0 if suite.result.wasSuccessful() else 1)
