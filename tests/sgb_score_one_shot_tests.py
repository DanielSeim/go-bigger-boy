#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical bounded one-shot BRR completion, gates and lifecycle replay."""
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
from build_sgb_score_one_shot_fixture import assets, bank, build as cartridge, SAMPLES, PROFILES, FAULTS
from schedule_sgb_score import schedule
PROBE = None


class OneShotTests(unittest.TestCase):
    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True, two_instruments=True, multiblock=True, instrument_profiles=True, one_shot=True))
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
                         (1, transfers, transfers + 1, 0xD1, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)



    def samples(self, result, sample_profile):
        counts, modes, loops = SAMPLES[sample_profile]
        self.assertEqual(result['sample_counts'], list(counts))
        self.assertEqual(result['sample_modes'], list(modes))
        self.assertEqual(result['sample_loops'], [0x5040 + 9*loops[0], 0x5080 + 9*loops[1]])

    def completion(self, result, sample_profile, sources=(2, 3, 3, 2)):
        modes = SAMPLES[sample_profile][1]
        self.assertEqual(result['end_windows'], 3)
        self.assertEqual(result['end_sources'], list(sources))
        self.assertEqual(result['end_kof'], [0, 0])
        self.assertEqual(result['endx'], [12, 12])
        mask = 0
        for index, source in enumerate(sources):
            self.assertGreater(result['end_gates'][index], 0)
            if modes[source-2]:
                self.assertEqual(result['end_env'][index], 0)
                mask |= 1 << ((index % 2)*2 + source-2)
            else:
                self.assertGreater(result['end_env'][index], 0)
        self.assertEqual(result['natural_ends'], mask)
        self.assertEqual(result['restore_ends'], mask)
        self.assertEqual(result['voice_env'], [0, 0])
        self.assertGreater(result['clocks'] - result['last_nonzero_clock'], 1000000)

    def test_caps_legacy_images_and_independent_schedule(self):
        options = dict(multisong=True, uploaded_instrument=True, two_instruments=True,
                       multiblock=True, instrument_profiles=True)
        self.assertEqual(hashlib.sha256(program(**options)).hexdigest(),
                         '951fbea38552e111403dd919f0892e37f75b0607cae95ce73731d93595e7c4e1')
        image = program(**options, one_shot=True)
        self.assertEqual(len(image), 262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         'b1f9b27f541165e18b48a660dd9473a859633374bea11645e7e664f3036639fb')
        self.assertEqual(image, program(**options, one_shot=True))
        for sample, (counts, modes, loops) in SAMPLES.items():
            data = assets(sample)
            self.assertEqual(len(data), 192)
            self.assertEqual(len(cartridge(sample_profile=sample)), 32768)
            for index, base in enumerate((64, 128)):
                self.assertEqual(data[19+4*index], modes[index])
                self.assertEqual(data[base+9*(counts[index]-1)], 0xB1 if modes[index] else 0xB3)
                self.assertEqual(data[base+9*counts[index]:base+64], bytes(64-9*counts[index]))
        for root, ticks in ((0x2B20, 72), (0x2B30, 72), (0x2B40, 76)):
            self.assertEqual(schedule(bank(), root, inherit_timing=True,
                                      end_priority=True, boundary_events=True)['ticks'], ticks)
        for options in ({'one_shot': True}, {**options, 'one_shot': 1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'sample_profile': 'unknown'}, {'profile': 'unknown'}, {'fault': 'unknown'},
                        {'sample_profile': 'pair', 'fault': 'mode2'}, {'order': ()},
                        {'order': (True,)}, {'active': 1}, {'repeat_upload': 1}):
            with self.assertRaises(ValueError):
                cartridge(**options)

    def test_silent_admission(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000)
            self.ready(result, 0, sounds=0, bridge=1)
            self.samples(result, 'intro')
            self.assertEqual(result['pcm']['nonzero_frames'], 0)
            self.assertEqual(result['natural_ends'], 0)

    def test_counts_natural_completion_mixed_sources_and_pcm_parity(self):
        for model in ('sgb', 'sgb2'):
            for sample in SAMPLES:
                baseline = None
                for mode in (('native', 'scalar', 'combined') if sample in
                             ('loop', 'one', 'full', 'mixed3') else ('native',)):
                    with self.subTest(model=model, sample=sample, mode=mode):
                        result = self.probe(model, sample_profile=sample, mode=mode)
                        self.ready(result, 1)
                        self.samples(result, sample)
                        self.completion(result, sample)
                        self.assertEqual(result['score_tick'], 72)
                        if sample == 'one':
                            # A terminal header alone can silence before audible data.
                            self.assertEqual(result['pcm']['nonzero_frames'], 0)
                        else:
                            self.assertGreater(result['pcm']['nonzero_frames'], 0)
                            self.assertEqual(result['instruments'], 15)
                        if baseline is None:
                            baseline = result
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames'] -
                                baseline['pcm']['frames'] * 3 // 2), 2)

    def test_uploaded_envelope_tuning_and_voice_local_instrument_events(self):
        for model in ('sgb', 'sgb2'):
            for profile in ('distinct', 'gain'):
                result = self.probe(model, profile=profile)
                self.ready(result, 1)
                self.completion(result, 'intro')
                self.assertEqual(result['profile_pitch'],
                                 [0x42C >> PROFILES[profile][i][3] for i in range(2)])
            for score, sources, final in (
                    ('inherit', (3, 2, 3, 3), (3, 3)), ('repeat', (2, 3, 3, 2), (3, 2)),
                    ('end', (2, 3, 3, 2), (3, 3)), ('clipped', (2, 3, 3, 2), (3, 2)),
                    ('all2', (2, 2, 2, 2), (2, 2)), ('all3', (3, 3, 3, 3), (3, 3)),
                    ('default', (2, 2, 3, 2), (3, 2))):
                with self.subTest(model=model, score=score):
                    result = self.probe(model, score_profile=score)
                    self.ready(result, 1)
                    self.completion(result, 'intro', sources)
                    self.assertEqual(result['score_tick'], 72)
                    self.assertEqual((result['source2'], result['source3']), final)

    def test_active_selection_stop_reupload_after_sample_end_and_during_loop(self):
        for model in ('sgb', 'sgb2'):
            for sample in ('intro', 'mixed3'):
                for order, repeat, mask in (((1, 2, 3), False, 2), ((2, 3), True, 4),
                                           ((2, 128, 3), False, 1)):
                    baseline = None
                    for mode in (('native', 'scalar', 'combined') if
                                 sample == 'mixed3' and order == (1, 2, 3) else ('native',)):
                        result = self.probe(model, order, active=True, repeat_upload=repeat,
                                            sample_profile=sample,
                                            clocks=75000000 if repeat else 45000000, mode=mode)
                        self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
                        self.samples(result, sample)
                        self.assertEqual(result['score_tick'], 76)
                        self.assertEqual(result['interruptions'], mask)
                        self.assertEqual(result['active_env'], mask if sample == 'mixed3' else 0)
                        self.assertGreater(result['interrupt_tick'], 0)
                        self.assertLess(result['interrupt_tick'], 14)
                        self.assertEqual(result['restore_commands'], mask)
                        if baseline is None:
                            baseline = result
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])

    def test_malformed_modes_headers_pointers_and_inherited_guards(self):
        for model in ('sgb', 'sgb2'):
            for fault in FAULTS:
                with self.subTest(model=model, fault=fault):
                    result = self.probe(model, fault=fault, clocks=30000000)
                    self.assertEqual((result['status'], result['adoptions'], result['version'],
                                      result['bridge'], result['signature'], result['admitted_roots']),
                                     (255, 1, 0, 0xE2, 0, 0))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)
                    self.assertEqual(result['natural_ends'], 0)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
