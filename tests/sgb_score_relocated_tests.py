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
from build_sgb_score_relocated_fixture import assets, bank, build as cartridge, BRR_PROFILES, FAULTS, LAYOUTS
from sgb_score_brr_profiles_tests import decoded_block, decoded_assets
from build_sgb_score_one_shot_fixture import SAMPLES
from build_sgb_score_instrument_profiles_fixture import PROFILES
from schedule_sgb_score import schedule
PROBE = None
DECODE_PROBE = None



class RelocatedTests(unittest.TestCase):
    def probe(self, model, order=(1,), clocks=45000000, mode='native', **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory) / 'host.rom', Path(directory) / 'game.gb'
            rom.write_bytes(program(multisong=True, uploaded_instrument=True, two_instruments=True, multiblock=True, instrument_profiles=True, one_shot=True, brr_profiles=True, relocatable=True))
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
                         (1, transfers, transfers + 1, 0xD3, bridge, 0xA5,
                          sounds, 0, 0, song, 3))
        self.assertEqual(result['restore_roots'], 7)



    def samples(self, result, sample_profile, brr_profile='mixed', layout='near'):
        data = assets(sample_profile, brr_profile=brr_profile, layout=layout)
        self.assertEqual(result['sample_starts'], [int.from_bytes(data[e:e+2], 'little') for e in (8, 12)])
        self.assertEqual(result['sample_loops'], [int.from_bytes(data[e:e+2], 'little') for e in (10, 14)])
        self.assertEqual(result['sample_counts'], list(data[16:18]))
        self.assertEqual(result['sample_modes'], [data[19], data[23]])
        self.assertEqual(result['sample_headers'], [data[b+9*n] for b in (64, 128) for n in range(4)])

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


    def test_caps_previous_image_and_reproducible_cli(self):
        options = dict(multisong=True, uploaded_instrument=True, two_instruments=True,
                       multiblock=True, instrument_profiles=True, one_shot=True, brr_profiles=True)
        self.assertEqual(hashlib.sha256(program(**options)).hexdigest(),
                         'a71cc1cec4e6bbc2c7220f144c601b65f20049683e2385c341156bd709cae1b2')
        image = program(**options, relocatable=True)
        self.assertEqual(len(image), 262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         '771bf03cfc3c1c091488fea1c0d26b9f78847edb970c9a7a44dab636d7618600')
        for bad in ({'relocatable': True}, {**options, 'relocatable': 1}):
            with self.assertRaises(ValueError):
                program(**bad)
        for bad in ({'layout': 'unknown'}, {'fault': 'unknown'}, {'layout': 'base', 'fault': 'mode2'},
                    {'sample_profile': 'loop', 'fault': 'mode2'}, {'active': 1}, {'order': (True,)}):
            with self.assertRaises(ValueError):
                cartridge(**bad)
        for root, ticks in ((0x2B20, 72), (0x2B30, 72), (0x2B40, 76)):
            self.assertEqual(schedule(bank(), root, inherit_timing=True,
                                      end_priority=True, boundary_events=True)['ticks'], ticks)
        with tempfile.TemporaryDirectory() as directory:
            for script, flags, expected in (
                    ('build_sgb_score_transport.py', ['--'+k.replace('_','-') for k in (*options, 'relocatable')], image),
                    ('build_sgb_score_relocated_fixture.py', ['--layout', 'last'], cartridge(layout='last'))):
                path = Path(directory)/script
                cmd = [sys.executable, str(ROOT/'scripts'/script), *flags, '--output', str(path)]
                child = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
                self.assertEqual(child.returncode, 0, child.stderr)
                self.assertEqual(path.read_bytes(), expected)
                self.assertNotEqual(subprocess.run(cmd, capture_output=True, timeout=10).returncode, 0)
                self.assertEqual(path.read_bytes(), expected)

    def test_all_aligned_starts_independent_decoded_sequences(self):
        if DECODE_PROBE is None:
            self.skipTest('decoder probe required')
        # Every allowed start/count pair, with both sources moving independently.
        for count in range(1, 5):
            sample = ('one', 'pair', 'triple', 'full')[count-1]
            for step in range((64-9*count)//9+1):
                for brr in BRR_PROFILES:
                    data = bytearray(assets(sample, brr_profile=brr, layout='base'))
                    expected = []
                    for source, window in enumerate((64, 128)):
                        shift = 9*(step if source == 0 else (64-9*count)//9-step)
                        chain = data[window:window+9*count]
                        data[window:window+64] = bytes(64)
                        data[window+shift:window+shift+9*count] = chain
                        for entry in (8+source*4, 10+source*4):
                            pointer = int.from_bytes(data[entry:entry+2], 'little')+shift
                            data[entry:entry+2] = pointer.to_bytes(2, 'little')
                        values, history = [], (0, 0)
                        for _ in range(2):
                            for block in range(count):
                                decoded, history = decoded_block(chain[9*block:9*block+9], history)
                                values.extend(decoded)
                        expected.append(values)
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory)/'asset.bin'
                        path.write_bytes(data)
                        child = subprocess.run([str(DECODE_PROBE), str(path)], capture_output=True, text=True, timeout=5)
                        self.assertEqual(child.returncode, 0, child.stderr)
                        self.assertEqual(json.loads(child.stdout), expected)

    def test_relocated_loop_reentry_matches_independent_arithmetic(self):
        if DECODE_PROBE is None:
            self.skipTest('decoder probe required')
        for sample in ('loop', 'mixed2', 'mixed3'):
            for brr in BRR_PROFILES:
                expected = decoded_assets(assets(sample, brr_profile=brr, layout='base'))
                for layout in LAYOUTS:
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory)/'asset.bin'
                        path.write_bytes(assets(sample, brr_profile=brr, layout=layout))
                        child = subprocess.run([str(DECODE_PROBE), str(path)], capture_output=True, text=True, timeout=5)
                        self.assertEqual(child.returncode, 0, child.stderr)
                        self.assertEqual(json.loads(child.stdout), expected)

    def test_silent_admission(self):
        for model in ('sgb', 'sgb2'):
            result = self.probe(model, clocks=18000000, layout='last')
            self.ready(result, 0, sounds=0, bridge=1)
            self.samples(result, 'loop', layout='last')
            self.assertEqual(result['pcm']['nonzero_frames'], 0)

    def test_relocated_playback_preserves_sample_completion(self):
        for model in ('sgb', 'sgb2'):
            for sample in ('loop', 'intro', 'mixed2', 'mixed3', 'full', 'one', 'pair', 'triple'):
                baseline = None
                for layout in LAYOUTS:
                    with self.subTest(model=model, sample=sample, layout=layout):
                        result = self.probe(model, sample_profile=sample, layout=layout)
                        self.ready(result, 1)
                        self.samples(result, sample, layout=layout)
                        self.completion(result, sample)
                        self.assertEqual(result['score_tick'], 72)
                        if baseline is None:
                            baseline = result
                        else:
                            self.assertEqual(result['pcm']['frames'], baseline['pcm']['frames'])
                            self.assertLessEqual(abs(result['pcm']['nonzero_frames']-baseline['pcm']['nonzero_frames']), 16)
                            self.assertLessEqual(abs(result['last_nonzero_clock']-baseline['last_nonzero_clock']), 50000)
                        if sample != 'one':
                            self.assertGreater(result['pcm']['nonzero_frames'], 0)

    def test_render_modes_and_voice_local_controls(self):
        for model in ('sgb', 'sgb2'):
            baseline = None
            for mode in ('native', 'scalar', 'combined'):
                result = self.probe(model, sample_profile='mixed3', layout='last', mode=mode)
                self.ready(result, 1)
                self.samples(result, 'mixed3', layout='last')
                self.completion(result, 'mixed3')
                if baseline is None:
                    baseline = result
                elif mode == 'scalar':
                    self.assertEqual(result['pcm'], baseline['pcm'])
                else:
                    self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2), 2)
            for score, sources, final in (
                    ('inherit', (3, 2, 3, 3), (3, 3)), ('repeat', (2, 3, 3, 2), (3, 2)),
                    ('end', (2, 3, 3, 2), (3, 3)), ('clipped', (2, 3, 3, 2), (3, 2)),
                    ('all2', (2, 2, 2, 2), (2, 2)), ('all3', (3, 3, 3, 3), (3, 3)),
                    ('default', (2, 2, 3, 2), (3, 2))):
                result = self.probe(model, score_profile=score, sample_profile='intro', layout='last', profile='distinct')
                self.ready(result, 1)
                self.completion(result, 'intro', sources)
                self.assertEqual((result['source2'], result['source3']), final)
                self.assertEqual(result['voice_tuning'], [PROFILES['distinct'][s-2][3] for s in final])

    def test_active_selection_stop_and_physical_reupload(self):
        for model in ('sgb', 'sgb2'):
            for order, repeat, mask in (((1, 2, 3), False, 2), ((2, 3), True, 4),
                                       ((2, 128, 3), False, 1)):
                for mode in (('native', 'scalar', 'combined') if order == (1, 2, 3) else ('native',)):
                    result = self.probe(model, order, active=True, repeat_upload=repeat,
                                        sample_profile='mixed3', layout='last',
                                        clocks=75000000 if repeat else 45000000, mode=mode)
                    self.ready(result, 3, transfers=2 if repeat else 1, sounds=2 if repeat else 6)
                    self.samples(result, 'mixed3', layout='last')
                    self.assertEqual(result['score_tick'], 76)
                    self.assertEqual(result['interruptions'], mask)
                    self.assertEqual(result['active_env'], mask)
                    self.assertGreater(result['interrupt_tick'], 0)
                    self.assertLess(result['interrupt_tick'], 14)
                    self.assertEqual(result['restore_commands'], mask)

    def test_bad_starts_loops_bounds_and_padding_reject_silently(self):
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
