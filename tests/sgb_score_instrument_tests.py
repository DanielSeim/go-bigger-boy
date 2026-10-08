#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical uploaded instrument mapping, guards and lifecycle with owned audio."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_sgb_score_transport import build as program
from build_sgb_score_instrument_fixture import instrument, build as cartridge, TIMBRES, FAULTS
from schedule_sgb_score import schedule
PROBE = None


class InstrumentTests(unittest.TestCase):
    def test_build_bounds_and_legacy_images(self):
        self.assertEqual(hashlib.sha256(program()).hexdigest(),
                         '220d292b4a41d07af3feb8e9375b57a78506b7a52bc2512d87ebc6de18d2850c')
        self.assertEqual(hashlib.sha256(program(multisong=True)).hexdigest(),
                         '72765eb405abe1b52eaa594b73d3e0403f072812e414828d07fffc8f924ffc3b')
        self.assertEqual(hashlib.sha256(program(multisong=True, uploaded_instrument=True)).hexdigest(),
                         '2ad4a5de1b707b27f5b09c08733dd7b5bdae49d411cfbffc1ed71279ed9a718b')
        for timbre in TIMBRES:
            self.assertEqual(len(instrument(timbre)), 32)
            self.assertEqual(len(cartridge(timbre=timbre)), 32768)
        for options in ({'uploaded_instrument': True}, {'multisong': True, 'uploaded_instrument': 1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'timbre': 'unknown'}, {'fault': 'unknown'}, {'order': ()},
                        {'order': (True,)}, {'active': 1}, {'repeat_upload': 1}):
            with self.assertRaises(ValueError):
                cartridge(**options)

    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True))
            game.write_bytes(cartridge(order, **fixture))
            child = subprocess.run([str(PROBE), str(rom), str(game), model, str(clocks), mode],
                                   capture_output=True, text=True, timeout=90)
            self.assertEqual(child.returncode, 0, child.stderr)
            self.assertLess(len(child.stdout), 4096)
            result = json.loads(child.stdout)
            self.assertEqual(result['schema'], 'gbb-score-transport-v1')
            self.assertIs(result['qualification'], False)
            self.assertIs(result['playback'], False)
            self.assertIs(result['reset_equal'], True)
            self.assertIs(result['restore_equal'], True)
            self.assertLessEqual(result['restore_count'], 4096)
            self.assertLessEqual(result['pcm']['frames'], 250000)
            self.assertLessEqual(result['clocks'] - clocks, 256)
            return result

    def ready(self, result, song, transfers=1, sounds=2, bridge=2):
        self.assertEqual((result['status'], result['transfers'], result['adoptions'],
                          result['version'], result['bridge'], result['signature'],
                          result['sounds'], result['error'], result['external'],
                          result['selected_song'], result['admitted_roots']),
                         (1, transfers, transfers + 1, 0xCD, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)

    def test_admission_is_silent(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000, timbre='step')
            self.ready(result, 0, sounds=0, bridge=1)
            self.assertEqual(result['pcm']['nonzero_frames'], 0)

    def test_distinct_uploaded_timbres_and_scalar_parity(self):
        for model in ('sgb', 'sgb2'):
            digests = set()
            for timbre in TIMBRES:
                baseline = None
                for mode in ('native', 'scalar', 'combined'):
                    with self.subTest(model=model, timbre=timbre, mode=mode):
                        result = self.probe(model, timbre=timbre, mode=mode)
                        self.ready(result, 1)
                        self.assertEqual(result['score_tick'], 72)
                        self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                        self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                        if baseline is None:
                            baseline = result
                            digests.add(result['pcm']['fnv1a64'])
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames'] - baseline['pcm']['frames'] * 3 // 2), 2)
            self.assertEqual(len(digests), 2)

    def test_active_switch_stop_and_upload(self):
        for model in ('sgb', 'sgb2'):
            for timbre in TIMBRES:
                for order, repeat, mask in (((1, 2, 3), False, 2), ((2, 3), True, 4), ((2, 128, 3), False, 1)):
                    result = self.probe(model, order, timbre=timbre, active=True,
                                        repeat_upload=repeat, clocks=75000000 if repeat else 45000000)
                    self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
                    self.assertEqual(result['score_tick'], 76)
                    self.assertEqual(result['interruptions'], mask)
                    self.assertEqual(result['active_env'], mask)
                    self.assertEqual(result['restore_commands'], mask)
                    self.assertGreater(result['pcm']['nonzero_frames'], 1000)

    def test_malformed_uploaded_assets_reject_before_any_song(self):
        for model in ('sgb', 'sgb2'):
            for fault in FAULTS:
                with self.subTest(model=model, fault=fault):
                    result = self.probe(model, fault=fault)
                    self.assertEqual((result['status'], result['adoptions'], result['version'],
                                      result['bridge'], result['signature'], result['admitted_roots']),
                                     (255, 1, 0, 0xE2, 0, 0))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
