#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical two-instrument prefix order, inheritance and bounded owned uploads."""
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
from build_sgb_score_dual_instrument_fixture import assets, bank, build as cartridge, PROFILES, FAULTS
from schedule_sgb_score import schedule
PROBE = None


class DualInstrumentTests(unittest.TestCase):
    def test_build_caps_and_independent_schedule(self):
        hashes = (({}, '220d292b4a41d07af3feb8e9375b57a78506b7a52bc2512d87ebc6de18d2850c'),
                  ({'multisong': True}, '72765eb405abe1b52eaa594b73d3e0403f072812e414828d07fffc8f924ffc3b'),
                  ({'multisong': True, 'uploaded_instrument': True},
                   '2ad4a5de1b707b27f5b09c08733dd7b5bdae49d411cfbffc1ed71279ed9a718b'),
                  ({'multisong': True, 'uploaded_instrument': True, 'two_instruments': True},
                   'ba442d2e0e6ae56d2db1d5295832683a907b79b05cdbab41efe1b640fe1d4802'))
        for options, expected in hashes:
            self.assertEqual(hashlib.sha256(program(**options)).hexdigest(), expected)
        self.assertEqual(len(assets()), 64)
        for profile in PROFILES:
            self.assertEqual(len(bank(profile)), 2048)
            self.assertEqual(len(cartridge(profile=profile)), 32768)
            for root, ticks in ((0x2B20, 72), (0x2B30, 72), (0x2B40, 76)):
                score = schedule(bank(profile), root, inherit_timing=True,
                                 end_priority=True, boundary_events=True)
                self.assertEqual(score['ticks'], ticks)
        for options in ({'two_instruments': True}, {'multisong': True, 'uploaded_instrument': True, 'two_instruments': 1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'profile': 'unknown'}, {'fault': 'unknown'}, {'swap': 1},
                        {'order': ()}, {'order': (True,)}, {'active': 1},
                        {'gate_boundary': True}, {'active': True, 'gate_boundary': 1}):
            with self.assertRaises(ValueError):
                cartridge(**options)

    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True, two_instruments=True))
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
                         (1, transfers, transfers + 1, 0xCE, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)

    def test_silent_admission(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000)
            self.ready(result, 0, sounds=0, bridge=1)
            self.assertEqual(result['pcm']['nonzero_frames'], 0)
            self.assertEqual(result['instruments'], 0)

    def test_ordered_selection_and_voice_local_inheritance(self):
        for model in ('sgb', 'sgb2'):
            digests = set()
            for profile, mask, sources, counts in (
                    ('switch', 15, (3, 2), (1, 1, 1, 1)),
                    ('inherit', 14, (3, 3), (1, 0, 1, 1)),
                    ('repeat', 15, (3, 2), (3, 1, 3, 1)),
                    ('maximum', 14, (3, 2), (26, 1, 27, 1)),
                    ('end', 15, (3, 3), (1, 1, 1, 1)),
                    ('clipped', 15, (3, 2), (1, 1, 1, 1)),
                    ('all2', 5, (2, 2), (1, 0, 1, 0)),
                    ('all3', 10, (3, 3), (1, 0, 1, 0))):
                baseline = None
                for mode in (('native', 'scalar', 'combined') if profile == 'switch' else ('native', 'scalar')):
                    with self.subTest(model=model, profile=profile, mode=mode):
                        result = self.probe(model, profile=profile, mode=mode)
                        self.ready(result, 1)
                        self.assertEqual(result['score_tick'], 72)
                        self.assertEqual(result['instruments'], mask)
                        self.assertEqual(result['restore_instruments'], mask)
                        self.assertEqual((result['source2'], result['source3']), sources)
                        self.assertEqual(tuple(result['prefix_counts'][:4]), counts)
                        # Later sparse/final patterns contain no E0: both DSP sources carry.
                        self.assertEqual(result['prefix_counts'][4:8], [0, 0, 0, 0])
                        self.assertEqual(result['prefix_counts'][8], 1 if profile == 'end' else 0)
                        if profile == 'end':
                            self.assertEqual(result['prefix_ids'][8], 3)
                        if profile == 'repeat':
                            self.assertEqual(result['prefix_ids'][:8], [2, 3, 2, 3, 3, 2, 3, 2])
                        self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                        self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                        if baseline is None:
                            baseline = result
                            digests.add(result['pcm']['fnv1a64'])
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames'] - baseline['pcm']['frames'] * 3 // 2), 2)
            self.assertGreaterEqual(len(digests), 4)

    def test_uploaded_wave_mapping_can_be_swapped(self):
        for model in ('sgb', 'sgb2'):
            digests = []
            for swap in (False, True):
                result = self.probe(model, profile='all3', swap=swap)
                self.ready(result, 1)
                self.assertEqual(result['instruments'], 10)
                self.assertEqual((result['source2'], result['source3']), (3, 3))
                digests.append(result['pcm']['fnv1a64'])
            self.assertNotEqual(*digests)

    def test_active_selection_stop_and_reupload(self):
        for model in ('sgb', 'sgb2'):
            for order, repeat, mask in (((1, 2, 3), False, 2), ((2, 3), True, 4), ((2, 128, 3), False, 1)):
                baseline = None
                for mode in (('native', 'scalar', 'combined') if order == (1, 2, 3) else ('native',)):
                    with self.subTest(model=model, order=order, mode=mode):
                        result = self.probe(model, order, active=True, repeat_upload=repeat,
                                            clocks=75000000 if repeat else 45000000, mode=mode)
                        self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
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

    def test_commands_at_gate_boundaries(self):
        for model in ('sgb', 'sgb2'):
            for repeat in (False, True):
                with self.subTest(model=model, repeat=repeat):
                    result = self.probe(model, (2, 3) if repeat else (1, 2, 3),
                                        active=True, repeat_upload=repeat, gate_boundary=True,
                                        clocks=75000000 if repeat else 45000000)
                    self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
                    self.assertEqual(result['interruptions'], 4 if repeat else 2)
                    # The same physical GB delay straddles the gate on SGB1
                    # and the following attack on SGB2; retain both outcomes.
                    self.assertEqual(result['active_env'], 2 if model == 'sgb2' and not repeat else 0)
                    self.assertGreaterEqual(result['interrupt_tick'], 14)
                    self.assertLessEqual(result['interrupt_tick'], 16)
                    self.assertEqual(result['score_tick'], 76)
                    self.assertEqual(result['instruments'], 15)
                    self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)

    def test_bad_assets_references_and_control_bounds(self):
        for model in ('sgb', 'sgb2'):
            for fault in FAULTS:
                with self.subTest(model=model, fault=fault):
                    result = self.probe(model, fault=fault)
                    admitted = 2 if fault == 'bad-third-id' else 0
                    self.assertEqual((result['status'], result['adoptions'], result['version'],
                                      result['bridge'], result['signature'], result['admitted_roots']),
                                     (255, 1, 0, 0xE2, 0, admitted))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)
                    self.assertEqual(result['instruments'], 0)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
