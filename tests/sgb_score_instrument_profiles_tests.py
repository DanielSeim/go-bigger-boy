#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical uploaded envelope/tuning profiles and voice-local lifecycle replay."""
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
from build_sgb_score_instrument_profiles_fixture import assets, build as cartridge, PROFILES, FAULTS
from schedule_sgb_score import schedule
PROBE = None


class InstrumentProfilesTests(unittest.TestCase):
    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True, two_instruments=True, multiblock=True, instrument_profiles=True))
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
                         (1, transfers, transfers + 1, 0xD0, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)


    def voice_profiles(self, result, profile, sources):
        expected = [field for source in sources for field in
                    (source, *PROFILES[profile][source-2][:3])]
        self.assertEqual(result['voice_profiles'], expected)
        self.assertEqual(result['voice_tuning'], [PROFILES[profile][source-2][3] for source in sources])
        self.assertEqual(result['voice_env'], [0, 0])
        # Last notes 33/27 include an odd pitch high byte, testing cross-byte carry.
        self.assertEqual(result['voice_pitch'], [pitch >> PROFILES[profile][source-2][3]
                         for pitch, source in zip((0x708, 0x4F8), sources)])

    def test_caps_and_reproducible_image(self):
        options = dict(multisong=True, uploaded_instrument=True, two_instruments=True,
                       multiblock=True, instrument_profiles=True)
        image = program(**options)
        self.assertEqual(len(image), 262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         '951fbea38552e111403dd919f0892e37f75b0607cae95ce73731d93595e7c4e1')
        self.assertEqual(image, program(**options))
        for profile, descriptors in PROFILES.items():
            data = assets(profile)
            self.assertEqual(len(data), 192)
            for index, (attack, sustain, gain, tuning) in enumerate(descriptors):
                self.assertEqual(data[4*index:4*index+4], bytes((2+index, attack, sustain, gain)))
                self.assertEqual(data[18+4*index], tuning)
            self.assertEqual(len(cartridge(profile=profile)), 32768)
        for options in ({'instrument_profiles': True}, {'multisong': True,
                        'uploaded_instrument': True, 'two_instruments': True,
                        'multiblock': True, 'instrument_profiles': 1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'profile': 'unknown'}, {'fault': 'unknown'}, {'order': ()},
                        {'order': (True,)}, {'active': 1}, {'repeat_upload': 1}):
            with self.assertRaises(ValueError):
                cartridge(**options)

    def test_silent_admission(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000)
            self.ready(result, 0, sounds=0, bridge=1)
            self.assertEqual(result['pcm']['nonzero_frames'], 0)

    def test_envelope_tuning_effects_and_pcm_parity(self):
        for model in ('sgb', 'sgb2'):
            results = {}
            for profile in PROFILES:
                with self.subTest(model=model, profile=profile):
                    result = self.probe(model, profile=profile)
                    self.ready(result, 1)
                    self.assertEqual(result['score_tick'], 72)
                    self.voice_profiles(result, profile, (3, 2))
                    self.assertEqual(result['instruments'], 15)
                    self.assertEqual(result['restore_instruments'], 15)
                    self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                    self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)
                    self.assertEqual(result['profile_pitch'],
                        [0x42C >> PROFILES[profile][i][3] for i in range(2)])
                    self.assertTrue(all(value > 0 for value in result['profile_env']))
                    results[profile] = result
            baseline = results['baseline']
            self.assertNotEqual(results['envelope']['profile_env'][0], baseline['profile_env'][0])
            self.assertEqual(results['envelope']['profile_env'][1], baseline['profile_env'][1])
            self.assertEqual(results['tuning']['profile_env'], baseline['profile_env'])
            self.assertEqual(results['gain']['profile_env'], [64, 96])
            for profile in ('envelope', 'tuning', 'distinct', 'swapped', 'gain'):
                self.assertNotEqual(results[profile]['pcm']['fnv1a64'], baseline['pcm']['fnv1a64'])
            for mode in ('scalar', 'combined'):
                result = self.probe(model, profile='distinct', mode=mode)
                self.ready(result, 1)
                self.assertEqual(result['profile_env'], results['distinct']['profile_env'])
                self.assertEqual(result['profile_pitch'], results['distinct']['profile_pitch'])
                if mode == 'scalar':
                    self.assertEqual(result['pcm'], results['distinct']['pcm'])
                else:
                    self.assertLessEqual(abs(result['pcm']['frames'] -
                        results['distinct']['pcm']['frames'] * 3 // 2), 2)

    def test_voice_local_inheritance_ordered_end_and_clipped_events(self):
        for model in ('sgb', 'sgb2'):
            for score, mask, sources in (('inherit', 14, (3, 3)), ('repeat', 15, (3, 2)),
                                         ('end', 15, (3, 3)), ('clipped', 15, (3, 2)),
                                         ('all2', 5, (2, 2)), ('all3', 10, (3, 3)),
                                         ('default', 7, (3, 2))):
                result = self.probe(model, score_profile=score)
                self.ready(result, 1)
                self.assertEqual(result['instruments'], mask)
                self.assertEqual((result['source2'], result['source3']), sources)
                self.voice_profiles(result, 'distinct', sources)
                # First-note instrument mapping is independent of final inheritance.
                first = (3, 2) if score == 'inherit' else (2, 3)
                if score == 'default':
                    first = (2, 2)
                if score.startswith('all'):
                    first = (int(score[-1]),) * 2
                self.assertEqual(result['profile_pitch'],
                    [0x42C >> PROFILES['distinct'][i-2][3] for i in first])

    def test_active_selection_stop_reupload(self):
        for model in ('sgb', 'sgb2'):
            for order, repeat, mask in (((1, 2, 3), False, 2), ((2, 3), True, 4),
                                       ((2, 128, 3), False, 1)):
                baseline = None
                for mode in (('native', 'scalar', 'combined') if order == (1, 2, 3) else ('native',)):
                    result = self.probe(model, order, active=True, repeat_upload=repeat,
                                        clocks=75000000 if repeat else 45000000, mode=mode)
                    self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
                    self.assertEqual(result['score_tick'], 76)
                    self.assertEqual(result['interruptions'], mask)
                    self.assertEqual(result['active_env'], mask)
                    self.assertGreater(result['interrupt_tick'], 0)
                    self.assertLess(result['interrupt_tick'], 14)
                    self.assertEqual(result['restore_commands'], mask)
                    if baseline is None:
                        baseline = result
                    elif mode == 'scalar':
                        self.assertEqual(result['pcm'], baseline['pcm'])

    def test_malformed_descriptors_tuning_and_reserved_data(self):
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
