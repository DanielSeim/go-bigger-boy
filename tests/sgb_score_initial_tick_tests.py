#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""D9 initial score tick fixes clipped timing while preserving frozen D8."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_score_async_adsr_tests as previous

IMAGE_HASH='b8d12f0632cb1893ff1518fbbbeb225c0077b80b7ab1fd93f72e8d7ff331939f'


class InitialTickTests(previous.AsyncAdsrTests):
    VERSION=0xD9
    BUILD_OPTIONS={**previous.AsyncAdsrTests.BUILD_OPTIONS,'initial_score_tick':True}
    EXPECTED_IMAGE_HASH=IMAGE_HASH
    RELEASE_PHASE_PROFILES=previous.AsyncAdsrTests.RELEASE_PHASE_PROFILES|{(10,'rests')}

    def trajectories(self,result,case,held_voice,slots):
        super().trajectories(result,case,held_voice,slots)
        timing=previous.reference_timing(result['envelope_notes'],case,held_voice)
        self.assertTrue(all(note['gate_matches'] and note['onset_matches'] for note in timing),timing)

    def test_d9_build_dependencies_caps_and_frozen_d8(self):
        image=previous.program(**self.BUILD_OPTIONS)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),IMAGE_HASH)
        self.assertEqual(hashlib.sha256(previous.program(**previous.AsyncAdsrTests.BUILD_OPTIONS)).hexdigest(),previous.prior.IMAGE_HASH)
        for flags in ({'initial_score_tick':True},{**self.BUILD_OPTIONS,'initial_score_tick':1},
                      {**self.BUILD_OPTIONS,'instrument_envelope':False}):
            with self.assertRaises(ValueError):
                previous.program(**flags)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'d9.rom'
            command=[sys.executable,str(previous.ROOT/'scripts/build_sgb_score_transport.py'),
                     *['--'+key.replace('_','-') for key in self.BUILD_OPTIONS],'--output',str(path)]
            for expected in (0,2):
                child=subprocess.run(command,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,expected,child.stderr)
                self.assertEqual(path.read_bytes(),image)

    def test_frozen_d8_clipped_gap_remains_distinct(self):
        for model in ('sgb','sgb2'):
            result=previous.AsyncAdsrTests().probe(model,'clipped',2)
            timing=previous.reference_timing(result['envelope_notes'],'clipped',2)
            self.assertFalse(timing[0]['gate_matches'])
            self.assertGreater(timing[0]['gate_spc_cycles'],176000)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining=parser.parse_known_args()
    previous.PROBE=args.probe.resolve() if args.probe else None
    suite=unittest.main(argv=[sys.argv[0],*remaining],exit=False)
    if previous.EVIDENCE:
        d9=[row for row in previous.EVIDENCE if row['version']==0xD9]
        print(json.dumps({'schema':'gbb-sgb-score-initial-tick-v1','qualification':False,'playback':False,
            'image_sha256':IMAGE_HASH,
            'suite_passed':suite.result.wasSuccessful(),'reference_timing_allowance_spc_cycles':4096,
            'timing_qualified':bool(d9) and all(note['gate_matches'] and note['onset_matches'] for row in d9 for note in row['timing']),
            'runs':d9,'d8_control_runs':[row for row in previous.EVIDENCE if row['version']==0xD8]},indent=2))
    sys.exit(0 if suite.result.wasSuccessful() else 1)
