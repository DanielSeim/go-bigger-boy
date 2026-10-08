#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned multi-song directory admission, physical selection and lifecycle."""
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
from build_sgb_score_directory_fixture import bank, build as cartridge, ROOTS, FAULTS
from schedule_sgb_score import schedule
PROBE = None


class DirectoryTests(unittest.TestCase):
    def test_reproducibility_independent_schedules_and_caps(self):
        self.assertEqual(hashlib.sha256(program()).hexdigest(),
                         '220d292b4a41d07af3feb8e9375b57a78506b7a52bc2512d87ebc6de18d2850c')
        self.assertEqual(hashlib.sha256(program(multisong=True)).hexdigest(),
                         '72765eb405abe1b52eaa594b73d3e0403f072812e414828d07fffc8f924ffc3b')
        self.assertEqual(len(bank()), 2048)
        self.assertEqual(len(cartridge((1, 2, 3), active=True)), 32768)
        schedules = [schedule(bank(), root, inherit_timing=True, end_priority=True,
                              boundary_events=True) for root in ROOTS]
        self.assertEqual([score['ticks'] for score in schedules], [72, 72, 76])
        self.assertEqual(len({json.dumps(score, sort_keys=True) for score in schedules}), 3)
        for options in ({'order': ()}, {'order': (True,)}, {'order': (256,)},
                        {'active': 1}, {'repeat_upload': 1}, {'fault': 'unknown'}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                cartridge(**options)
        with self.assertRaises(ValueError):
            program(multisong=1)

    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True))
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
                         (1, transfers, transfers + 1, 0xCC, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)

    def test_all_roots_validate_silently_before_readiness(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000)
            self.ready(result, 0, sounds=0, bridge=1)
            self.assertEqual(result['pcm']['nonzero_frames'], 0)
            self.assertEqual(result['restore_phases'] & 7, 7)

    def test_each_song_is_audibly_distinct_and_has_exact_scalar_parity(self):
        for model in ('sgb', 'sgb2'):
            digests = set()
            for song, ticks in ((1, 72), (2, 72), (3, 76)):
                baseline = None
                for mode in ('native', 'scalar', 'combined'):
                    with self.subTest(model=model, song=song, mode=mode):
                        result = self.probe(model, (song,), mode=mode)
                        self.ready(result, song)
                        self.assertEqual(result['score_tick'], ticks)
                        self.assertEqual(result['interruptions'], 0)
                        self.assertEqual(result['restore_phases'], 31)
                        self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                        self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                        if baseline is None:
                            baseline = result
                            digests.add(result['pcm']['fnv1a64'])
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames'] -
                                                     baseline['pcm']['frames'] * 3 // 2), 2)
            self.assertEqual(len(digests), 3)

    def test_active_switch_order_stop_resume_and_repeat_upload(self):
        for model in ('sgb', 'sgb2'):
            for order, repeat, mask, count in (((1, 2, 3), False, 2, 2),
                                               ((3, 2, 1), False, 2, 2),
                                               ((2, 0x80, 3), False, 1, 1),
                                               ((2, 3), True, 4, 1)):
                baseline = None
                for mode in (('native', 'scalar', 'combined') if order == (1, 2, 3) else ('native',)):
                    with self.subTest(model=model, order=order, repeat=repeat, mode=mode):
                        result = self.probe(model, order, clocks=75000000 if repeat else 45000000,
                                            active=True, repeat_upload=repeat, mode=mode)
                        self.ready(result, order[-1], transfers=2 if repeat else 1,
                                   sounds=2 if repeat else 2 * len(order))
                        self.assertEqual(result['score_tick'], 76 if order[-1] == 3 else 72)
                        self.assertEqual(result['interruptions'], mask)
                        self.assertEqual(result['restore_commands'], mask)
                        self.assertEqual(result['active_env'], mask)
                        self.assertEqual(result['interrupt_count'], count)
                        self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                        if baseline is None:
                            baseline = result
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])

    def test_invalid_ids_stop_and_retain_admitted_directory(self):
        for model in ('sgb', 'sgb2'):
            for song in (0, 4, 255):
                with self.subTest(model=model, song=song):
                    result = self.probe(model, (song,))
                    self.assertEqual((result['status'], result['error'], result['external'],
                                      result['admitted_roots']), (255, 0, 0, 3))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)

    def test_bad_unselected_third_root_prevents_adoption_and_audio(self):
        for model in ('sgb', 'sgb2'):
            for fault in FAULTS:
                with self.subTest(model=model, fault=fault):
                    result = self.probe(model, (1,), fault=fault)
                    self.assertEqual((result['status'], result['transfers'], result['adoptions'],
                                      result['version'], result['bridge'], result['signature'],
                                      result['external'], result['admitted_roots']),
                                     (255, 1, 1, 0, 0xE2, 0, 1, 2))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)
                    self.assertEqual(result['restore_roots'], 3)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
