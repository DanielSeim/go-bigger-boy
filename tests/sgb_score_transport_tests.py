#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public whole-host owned score integration; no proprietary input required."""
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
from build_sgb_score_transport_fixture import build as cartridge
from build_sgb_song_selection_fixture import build_cartridge, score_payload

PROBE = None


class ScoreTransportTests(unittest.TestCase):
    def test_reproducibility_bounds_and_default_fixture(self):
        image = program()
        self.assertEqual(len(image), 262144)
        self.assertEqual(image, program())
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         '0cf3e20bb7386d3862260dad338e51da4153305380182f92b0c182981fe10db8')
        self.assertEqual(cartridge(), cartridge())
        self.assertEqual(len(cartridge((1, 1), repeat_upload=True)), 32768)
        from build_sgb_song_selection_fixture import build
        self.assertEqual(build(), build_cartridge(score_payload(), (1, 2, 3)))
        for options in ({'wait_frames': 15}, {'wait_frames': 129}, {'wait_frames': True},
                        {'repeat_upload': 1}, {'sound_fields': (0, 0)},
                        {'sound_fields': (True, 0, 0)}, {'sound_fields': (256, 0, 0)}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                build_cartridge(score_payload(), (1,), **options)

    def probe(self, model, clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom = Path(directory) / 'host.rom'
            game = Path(directory) / 'game.gb'
            rom.write_bytes(program())
            game.write_bytes(cartridge(**fixture))
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
            self.assertGreater(result['restore_count'], 0)
            self.assertLessEqual(result['restore_count'], 4096)
            self.assertLessEqual(result['pcm']['frames'], 250000)
            self.assertLessEqual(result['clocks'] - clocks, 256)
            return result

    def ready(self, result, transfers=1, sounds=2, bridge=2):
        self.assertEqual((result['status'], result['transfers'], result['adoptions'],
                          result['version'], result['bridge'], result['signature'],
                          result['sounds'], result['error'], result['external']),
                         (1, transfers, transfers + 1, 0xCB, bridge, 0xA5, sounds, 0, 0))

    def test_restart_validates_silently_before_sound(self):
        for model in ('sgb', 'sgb2'):
            with self.subTest(model=model):
                result = self.probe(model, clocks=18000000)
                self.ready(result, sounds=0, bridge=1)
                self.assertEqual(result['pcm']['nonzero_frames'], 0)
                self.assertEqual(result['restore_phases'] & 7, 7)

    def test_first_selection_audio_modes_and_lifecycle(self):
        for model in ('sgb', 'sgb2'):
            baseline = None
            for mode in ('native', 'scalar', 'combined'):
                with self.subTest(model=model, mode=mode):
                    result = self.probe(model, mode=mode)
                    self.ready(result)
                    self.assertEqual(result['restore_phases'], 31)
                    self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                    self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                    if baseline is None:
                        baseline = result
                    elif mode == 'scalar':
                        self.assertEqual(result['pcm'], baseline['pcm'])
                        self.assertEqual(result['last_nonzero_clock'], baseline['last_nonzero_clock'])
                    else:
                        # Combined output resamples the 32 kHz DSP stream to 48 kHz.
                        self.assertLessEqual(abs(result['pcm']['frames'] -
                                                 baseline['pcm']['frames'] * 3 // 2), 2)
                        self.assertLessEqual(abs(result['pcm']['nonzero_frames'] -
                                                 baseline['pcm']['nonzero_frames'] * 3 // 2), 16)

    def test_reselection_and_repeated_upload(self):
        for model in ('sgb', 'sgb2'):
            for repeat in (False, True):
                with self.subTest(model=model, repeat=repeat):
                    result = self.probe(model, clocks=100000000 if repeat else 75000000,
                                        order=(1, 1), repeat_upload=repeat)
                    self.ready(result, transfers=2 if repeat else 1, sounds=2 if repeat else 4)
                    self.assertGreater(result['pcm']['nonzero_frames'], 20000)
                    self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                    self.assertEqual(result['restore_phases'], 31)

    def test_invalid_bank_does_not_advertise_readiness(self):
        for model in ('sgb', 'sgb2'):
            with self.subTest(model=model):
                result = self.probe(model, malformed=True)
                self.assertEqual((result['status'], result['transfers'], result['adoptions'],
                                  result['version'], result['bridge'], result['signature'],
                                  result['external']), (0xFF, 1, 1, 0, 0xE2, 0, 1))
                self.assertEqual(result['pcm']['nonzero_frames'], 0)

    def test_unsupported_songs_effects_and_attributes_stop_silently(self):
        for model in ('sgb', 'sgb2'):
            for options in ({'order': (2,)}, {'order': (3,)},
                            {'sound_fields': (1, 0, 0)}, {'sound_fields': (0, 1, 0)},
                            {'sound_fields': (0, 0, 1)}):
                with self.subTest(model=model, options=options):
                    result = self.probe(model, **options)
                    self.assertEqual((result['status'], result['transfers'], result['adoptions'],
                                      result['external'], result['error']), (0xFF, 1, 2, 0, 0))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
