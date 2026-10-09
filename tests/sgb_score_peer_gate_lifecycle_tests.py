#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DA bounded gate calibration across real replacement, STOP and recovery."""
import argparse
from pathlib import Path
import unittest
import sys
import sgb_score_adsr_tests as prior


class PeerGateLifecycleTests(unittest.TestCase):
    VERSION=0xDA
    EXTRA_OPTIONS={'initial_score_tick':True,'peer_gate_timing':True}
    probe=prior.AdsrTests.probe
    ready=prior.AdsrTests.ready
    regions=prior.AdsrTests.regions
    voices=prior.AdsrTests.voices
    test_active_replacement_stop_and_reupload=prior.AdsrTests.test_active_replacement_stop_and_reupload
    test_fragmented_cold_and_active_rejection_recovery=prior.AdsrTests.test_fragmented_cold_and_active_rejection_recovery


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    prior.PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
