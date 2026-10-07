#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measured two-voice ADSR setup, envelope lifecycle and mix regressions."""
import argparse
import copy
import hashlib
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import sgb_score_mix_tests as mix
from build_sgb_score_envelope import build
from build_sgb_score_mix import build as prior_build
from build_sgb_chromatic_fixture import bank as chromatic_bank
from check_sgb_score_envelope_reference import native, align, validate, compare
from check_sgb_score_polygate_reference import native as gate_native

# Run the complete existing mix/lifecycle contract with the ADSR driver.
# Each inherited test uses this module's selected probe via mix's globals.
mix.native, mix.align, mix.validate, mix.compare = native, align, validate, compare


class EnvelopeTests(mix.MixTests):
    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(), '4cf02fde17d9af55a3583fc5d643fe09cee04b0828694fbd1c630a092abb8e1c')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(), '210fec34a40227286ae3bc5c6006b43580cb4d537c228c2fea4c0f17d28f6667')
        image = build()
        self.assertEqual(image[0x1008-0x800:0x100C-0x800], bytes((0x10,)*4))
        self.assertEqual(image[0x1010-0x800:0x1019-0x800], prior_build()[0x1010-0x800:0x1019-0x800])

    def test_endpoints_and_volume_pcm(self):
        peaks = []
        stream = (16,127,0x98,0xA4,0)
        # ADSR decay uses the global rate clock. Different command prefix
        # lengths can shift peak PCM slightly despite identical volume pairs.
        for prefix in ((0xE1,10),(0xE1,10,0xED,64),(0xE5,80,0xE1,10)):
            streams = ((*prefix,*stream),)*4 if 0xE5 not in prefix else ((*prefix,*stream),stream,stream,stream)
            data = mix.authored(streams)
            report = native(mix.PROBE,data)
            align(report,data)
            self.assertTrue(report['pcm']['stereo_equal'])
            peaks.append(report['pcm']['peak'])
        self.assertGreater(peaks[0],peaks[1])
        self.assertGreater(peaks[0],peaks[2])
        for pan,lane in ((0,'left'),(20,'right')):
            stream = (0xE1,pan,16,63,0x9A,0xA3,0)
            data = mix.authored((stream,)*4)
            report = native(mix.PROBE,data)
            align(report,data)
            self.assertEqual(report['pcm'][lane+'_nonzero_frames'],0)
            self.assertEqual(report['pcm'][lane+'_peak'],0)

    def test_every_octave_note_attacks_decays_and_releases(self):
        for art in (63, 127):
            data = chromatic_bank(art)
            report = native(mix.PROBE, data)
            align(report, data)
            self.assertEqual(len(report['envelopes']), 26)
            for env in report['envelopes']:
                self.assertTrue(env['attack_zero'])
                self.assertEqual(env['peak'], 127)
                self.assertLess(env['decay_min'], 127)
                self.assertTrue(env['release_zero'])
                self.assertGreater(env['release_steps'], 0)

    def test_envelope_metadata_and_register_guards(self):
        report = native(mix.PROBE, mix.bank(63))
        for key, value in (('peak',126),('attack_zero',False),('decay_min',127),('release_zero',False),
                           ('release_steps',0),('off_half_cycle',0),('voice',True)):
            changed = copy.deepcopy(report)
            changed['envelopes'][0][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        for name in ('keyons', 'keyoffs'):
            changed = copy.deepcopy(report)
            changed[name][0]['instrument_setup'][1][1] = 0
            with self.assertRaises(ValueError):
                validate(changed)
        for altered in ([], [None], report['envelopes'][:-1], report['envelopes']+[report['envelopes'][0]]):
            changed = copy.deepcopy(report)
            changed['envelopes'] = altered
            with self.assertRaises(ValueError):
                validate(changed)
        # The actual component probe rejects direct gain, even if host metadata
        # or a mistaken builder claims this is an envelope-enabled image.
        with self.assertRaises(ValueError):
            gate_native(mix.PROBE, mix.bank(63), builder=prior_build, validator=validate)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    mix.PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
