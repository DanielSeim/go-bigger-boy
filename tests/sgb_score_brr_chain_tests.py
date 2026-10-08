#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical bounded multi-block BRR loops, upload guards and lifecycle replay."""
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
from build_sgb_score_brr_chain_fixture import assets, bank, build as cartridge, SAMPLES, FAULTS
from schedule_sgb_score import schedule
PROBE = None


class BrrChainTests(unittest.TestCase):
    def test_caps_legacy_images_and_independent_score_schedule(self):
        hashes = (({}, '220d292b4a41d07af3feb8e9375b57a78506b7a52bc2512d87ebc6de18d2850c'),
                  ({'multisong': True}, '72765eb405abe1b52eaa594b73d3e0403f072812e414828d07fffc8f924ffc3b'),
                  ({'multisong': True, 'uploaded_instrument': True},
                   '2ad4a5de1b707b27f5b09c08733dd7b5bdae49d411cfbffc1ed71279ed9a718b'),
                  ({'multisong': True, 'uploaded_instrument': True, 'two_instruments': True},
                   'ba442d2e0e6ae56d2db1d5295832683a907b79b05cdbab41efe1b640fe1d4802'),
                  ({'multisong': True, 'uploaded_instrument': True, 'two_instruments': True, 'multiblock': True},
                   'bfc7f800a706012540a735bf8ec4284e8379ddbe555d2590101dd3e0c20fe4fb'))
        for options, expected in hashes:
            self.assertEqual(hashlib.sha256(program(**options)).hexdigest(), expected)
        for profile in SAMPLES:
            self.assertEqual(len(assets(profile)), 192)
            self.assertEqual(len(cartridge(sample_profile=profile)), 32768)
        for root, ticks in ((0x2B20, 72), (0x2B30, 72), (0x2B40, 76)):
            self.assertEqual(schedule(bank(), root, inherit_timing=True,
                                      end_priority=True, boundary_events=True)['ticks'], ticks)
        for options in ({'multiblock': True}, {'multisong': True, 'uploaded_instrument': True,
                                             'two_instruments': True, 'multiblock': 1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'sample_profile': 'unknown'}, {'score_profile': 'unknown'},
                        {'fault': 'unknown'}, {'sample_profile': 'single', 'fault': 'count-zero'},
                        {'order': ()}, {'order': (True,)}, {'active': 1}, {'repeat_upload': 1}):
            with self.assertRaises(ValueError):
                cartridge(**options)

    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True, two_instruments=True, multiblock=True))
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
                         (1, transfers, transfers + 1, 0xCF, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)

    def samples(self, result, profile):
        counts, loops = SAMPLES[profile]
        self.assertEqual(result['sample_counts'], list(counts))
        self.assertEqual(result['sample_loops'], [0x5040 + 9 * loops[0], 0x5080 + 9 * loops[1]])

    def test_silent_admission_of_both_chains(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000)
            self.ready(result, 0, sounds=0, bridge=1)
            self.samples(result, 'intro')
            self.assertEqual(result['pcm']['nonzero_frames'], 0)
            self.assertEqual(result['instruments'], 0)

    def test_block_counts_loop_points_pcm_and_scalar_parity(self):
        for model in ('sgb', 'sgb2'):
            digests = set()
            for profile in SAMPLES:
                baseline = None
                for mode in ('native', 'scalar', 'combined'):
                    with self.subTest(model=model, profile=profile, mode=mode):
                        result = self.probe(model, sample_profile=profile, mode=mode)
                        self.ready(result, 1)
                        self.samples(result, profile)
                        self.assertEqual(result['score_tick'], 72)
                        self.assertEqual(result['instruments'], 15)
                        self.assertEqual(result['restore_instruments'], 15)
                        self.assertEqual((result['source2'], result['source3']), (3, 2))
                        self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                        self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                        if baseline is None:
                            baseline = result
                            digests.add(result['pcm']['fnv1a64'])
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames'] - baseline['pcm']['frames'] * 3 // 2), 2)
            # Includes identical four-block data with first-vs-last-block loops.
            self.assertEqual(len(digests), len(SAMPLES))

    def test_inherited_ordered_end_and_clipped_instrument_events(self):
        for model in ('sgb', 'sgb2'):
            for profile, mask, sources in (('inherit', 14, (3, 3)), ('repeat', 15, (3, 2)),
                                            ('end', 15, (3, 3)), ('clipped', 15, (3, 2))):
                with self.subTest(model=model, profile=profile):
                    result = self.probe(model, score_profile=profile)
                    self.ready(result, 1)
                    self.samples(result, 'intro')
                    self.assertEqual(result['score_tick'], 72)
                    self.assertEqual(result['instruments'], mask)
                    self.assertEqual(result['restore_instruments'], mask)
                    self.assertEqual((result['source2'], result['source3']), sources)
                    if profile == 'repeat':
                        self.assertEqual(result['prefix_ids'][:8], [2, 3, 2, 3, 3, 2, 3, 2])
                    self.assertEqual(result['prefix_counts'][4:8], [0, 0, 0, 0])

    def test_active_switch_stop_and_reupload(self):
        for model in ('sgb', 'sgb2'):
            for order, repeat, mask in (((1, 2, 3), False, 2), ((2, 3), True, 4), ((2, 128, 3), False, 1)):
                baseline = None
                for mode in (('native', 'scalar', 'combined') if order == (1, 2, 3) else ('native',)):
                    with self.subTest(model=model, order=order, mode=mode):
                        result = self.probe(model, order, active=True, repeat_upload=repeat,
                                            clocks=75000000 if repeat else 45000000, mode=mode)
                        self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
                        self.samples(result, 'intro')
                        self.assertEqual(result['score_tick'], 76)
                        self.assertEqual(result['instruments'], 15)
                        self.assertEqual(result['restore_instruments'], 15)
                        self.assertEqual(result['interruptions'], mask)
                        self.assertEqual(result['active_env'], mask)
                        self.assertGreater(result['interrupt_tick'], 0)
                        self.assertLess(result['interrupt_tick'], 14)
                        self.assertEqual(result['restore_commands'], mask)
                        if baseline is None:
                            baseline = result
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])

    def test_malformed_chains_counts_loops_and_padding_reject_silently(self):
        for model in ('sgb', 'sgb2'):
            for fault in FAULTS:
                with self.subTest(model=model, fault=fault):
                    result = self.probe(model, fault=fault, clocks=30000000)
                    self.assertEqual((result['status'], result['adoptions'], result['version'],
                                      result['bridge'], result['signature'], result['admitted_roots']),
                                     (255, 1, 0, 0xE2, 0, 0))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)
                    self.assertEqual(result['instruments'], 0)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
