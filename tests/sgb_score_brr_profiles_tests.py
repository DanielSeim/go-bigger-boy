#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded BRR filters/ranges, independent decoded samples and physical lifecycle."""
import argparse
import hashlib
from fractions import Fraction
from math import floor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_sgb_score_transport import build as program
from build_sgb_score_brr_profiles_fixture import assets, bank, build as cartridge, BRR_PROFILES, FAULTS
from build_sgb_score_one_shot_fixture import SAMPLES
from build_sgb_score_instrument_profiles_fixture import PROFILES
from schedule_sgb_score import schedule
PROBE = None
DECODE_PROBE = None



def decoded_block(block, history=(0, 0)):
    # A separate rational form: round each predictor contribution before sum.
    weights = ((Fraction(0), Fraction(0)), (Fraction(15,16), Fraction(0)),
               (Fraction(61,32), Fraction(-15,16)), (Fraction(115,64), Fraction(-13,16)))
    r, f = block[0] >> 4, (block[0] >> 2) & 3
    p, q = history
    out = []
    for byte in block[1:]:
        for nibble in (byte >> 4, byte & 15):
            signed = nibble if nibble < 8 else nibble-16
            raw = signed * 2**(r-1) + floor(weights[f][0]*p) + floor(weights[f][1]*q)
            clamped = min(32767, max(-32768, raw))
            value = (clamped + 16384) % 32768 - 16384
            out.append(value)
            p, q = value, p
    return out, (p, q)


def decoded_assets(data):
    result = []
    for source, base in enumerate((64, 128)):
        count = data[16+source]
        loop = (int.from_bytes(data[10+4*source:12+4*source], 'little')-0x5000-base)//9
        values, history = [], (0, 0)
        for start in (0, loop):
            for block in range(start, count):
                offset = base+9*block
                decoded, history = decoded_block(data[offset:offset+9], history)
                values.extend(decoded)
        result.append(values)
    return result

class BrrProfilesTests(unittest.TestCase):
    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True, two_instruments=True, multiblock=True, instrument_profiles=True, one_shot=True, brr_profiles=True))
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
                         (1, transfers, transfers + 1, 0xD2, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)



    def samples(self, result, sample_profile, brr_profile='mixed'):
        data = assets(sample_profile, brr_profile=brr_profile)
        self.assertEqual(result['sample_headers'], [data[base+9*n] for base in (64, 128) for n in range(4)])
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


    def test_independent_arithmetic_vectors_and_decoder(self):
        for f, positive, negative in ((0, [128, 0, 0], [-128, 0, 0]),
                                     (1, [128, 120, 112], [-128, -120, -113]),
                                     (2, [128, 244, 345], [-128, -244, -346]),
                                     (3, [128, 230, 309], [-128, -230, -310])):
            for nibble, expected in ((0x10, positive), (0xF0, negative)):
                block = bytes(((8 << 4) | (f << 2), nibble)) + bytes(7)
                self.assertEqual(decoded_block(block)[0][:3], expected)
        if DECODE_PROBE is None:
            self.skipTest('decoder probe required')
        for brr in BRR_PROFILES:
            for sample in ('one', 'pair', 'loop', 'intro', 'mixed2', 'full'):
                data = assets(sample, brr_profile=brr)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory)/'asset.bin'
                    path.write_bytes(data)
                    child = subprocess.run([str(DECODE_PROBE), str(path)], capture_output=True, text=True, timeout=5)
                    self.assertEqual(child.returncode, 0, child.stderr)
                    self.assertLess(len(child.stdout), 8192)
                    self.assertEqual(json.loads(child.stdout), decoded_assets(data))

    def test_caps_legacy_images_and_independent_schedule(self):
        options = dict(multisong=True, uploaded_instrument=True, two_instruments=True,
                       multiblock=True, instrument_profiles=True, one_shot=True)
        self.assertEqual(hashlib.sha256(program(**options)).hexdigest(),
                         'b1f9b27f541165e18b48a660dd9473a859633374bea11645e7e664f3036639fb')
        image = program(**options, brr_profiles=True)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         'a71cc1cec4e6bbc2c7220f144c601b65f20049683e2385c341156bd709cae1b2')
        self.assertEqual(len(image), 262144)
        for root, ticks in ((0x2B20, 72), (0x2B30, 72), (0x2B40, 76)):
            self.assertEqual(schedule(bank(), root, inherit_timing=True,
                                      end_priority=True, boundary_events=True)['ticks'], ticks)
        for options in ({'brr_profiles': True}, {**options, 'brr_profiles': 1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'brr_profile': 'unknown'}, {'sample_profile': 'unknown'}, {'fault': 'unknown'},
                        {'sample_profile': 'loop', 'fault': 'mode2'}, {'active': 1}, {'order': (True,)}):
            with self.assertRaises(ValueError):
                cartridge(**options)

    def test_silent_admission(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000)
            self.ready(result, 0, sounds=0, bridge=1)
            self.samples(result, 'loop')
            self.assertEqual(result['pcm']['nonzero_frames'], 0)

    def test_all_filters_ranges_and_block_transitions_looping_pcm(self):
        for model in ('sgb', 'sgb2'):
            digests = set()
            for brr in BRR_PROFILES:
                baseline = None
                for mode in (('native', 'scalar', 'combined') if brr in ('f3r11', 'mixed', 'swap') else ('native',)):
                    with self.subTest(model=model, brr=brr, mode=mode):
                        result = self.probe(model, brr_profile=brr, mode=mode)
                        self.ready(result, 1)
                        self.samples(result, 'loop', brr)
                        self.completion(result, 'loop')
                        self.assertEqual(result['score_tick'], 72)
                        self.assertEqual(result['instruments'], 15)
                        self.assertGreater(result['pcm']['nonzero_frames'], 1000)
                        if baseline is None:
                            baseline = result
                            digests.add(result['pcm']['fnv1a64'])
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'], baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames'] - baseline['pcm']['frames']*3//2), 2)
            self.assertEqual(len(digests), len(BRR_PROFILES))

    def test_one_shot_and_mixed_modes_with_predictive_filters(self):
        for model in ('sgb', 'sgb2'):
            for brr in BRR_PROFILES[:8]:
                result = self.probe(model, brr_profile=brr, sample_profile='intro')
                self.ready(result, 1)
                self.samples(result, 'intro', brr)
                self.completion(result, 'intro')
                self.assertGreater(result['pcm']['nonzero_frames'], 0)
            for sample in ('mixed2', 'mixed3'):
                baseline = None
                for mode in ('native', 'scalar', 'combined'):
                    result = self.probe(model, sample_profile=sample, mode=mode)
                    self.ready(result, 1)
                    self.samples(result, sample)
                    self.completion(result, sample)
                    if baseline is None:
                        baseline = result
                    elif mode == 'scalar':
                        self.assertEqual(result['pcm'], baseline['pcm'])

    def test_uploaded_envelope_tuning_and_voice_local_instrument_events(self):
        for model in ('sgb', 'sgb2'):
            for profile in ('distinct', 'gain'):
                result = self.probe(model, profile=profile, sample_profile='intro')
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
                    result = self.probe(model, score_profile=score, sample_profile='intro')
                    self.ready(result, 1)
                    self.completion(result, 'intro', sources)
                    self.assertEqual(result['score_tick'], 72)
                    self.assertEqual((result['source2'], result['source3']), final)

    def test_active_selection_stop_reupload_after_sample_end_and_during_loop(self):
        for model in ('sgb', 'sgb2'):
            for sample in ('mixed3',):
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
                    result = self.probe(model, fault=fault, sample_profile='intro', clocks=30000000)
                    self.assertEqual((result['status'], result['adoptions'], result['version'],
                                      result['bridge'], result['signature'], result['admitted_roots']),
                                     (255, 1, 0, 0xE2, 0, 0))
                    self.assertEqual(result['pcm']['nonzero_frames'], 0)
                    self.assertEqual(result['natural_ends'], 0)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--decode-probe', type=Path)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    DECODE_PROBE = args.decode_probe.resolve() if args.decode_probe else None
    unittest.main(argv=[sys.argv[0], *remaining])
