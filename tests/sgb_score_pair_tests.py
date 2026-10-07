#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free native two-track first-end scheduling and lifecycle checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_pair import build
from build_sgb_phrase_fixture import CASES, score_payload
from check_sgb_score_pair_reference import align, compare, fixture, native, validate
from schedule_sgb_score import schedule

PROBE = None


def symbolic(pair):
    # Independently encode the diagnostic events as ordinary N-SPC track streams.
    bank = bytearray(48)
    struct.pack_into('<HHH', bank, 0, 0x2B10, 0x2B20, 0)
    for pattern in range(2):
        for channel in (2, 3):
            offset = pattern * 4 + (channel-2) * 2
            struct.pack_into('<H', bank, 16 + pattern*16 + channel*2, 0x2B00+len(bank))
            bank.extend((pair[offset], 127, pair[offset+1], 0))
    return schedule(bytes(bank), 0x2B00)


class PairTests(unittest.TestCase):
    def test_reproducible(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '89ebba0a2f780d6158f786e47c913e5751b64c3d742868ca08e891b27763f7dd')

    def test_reference_fixture_first_end_and_clipping(self):
        for case in CASES:
            with self.subTest(case=case):
                payload = score_payload(case)
                bank = payload[4:4+int.from_bytes(payload[:2], 'little')]
                score = schedule(bank, 0x2B10)
                align(native(PROBE, fixture(case)), score)
                if case != 'both-long':
                    self.assertEqual(len(score['patterns'][0]['truncated_channels']), 1)

    def test_rest_boundaries_and_tempo(self):
        pairs = (bytes((1, 0x80, 127, 0xC7, 127, 0xC9, 2, 0x98)),
                 bytes((127, 0xC9, 127, 0xC9, 127, 0x80, 127, 0xC7)),
                 bytes((32, 0x99, 16, 0x98, 7, 0xC9, 19, 0xA4)))
        for pair in pairs:
            reports = [native(PROBE, pair, tempo) for tempo in (96, 192)]
            for report in reports:
                align(report, symbolic(pair))
            slow, fast = [(r['events'][2]['half_cycle']-r['events'][0]['half_cycle'])/2
                          for r in reports]
            # Allow one timer quantum plus bounded event-write latency.
            self.assertLess(abs(slow-2*fast), 4096)

    def test_reject_before_events(self):
        pair = fixture('short-first')
        invalid = [pair[:n] for n in range(1, 8)] + [pair+b'\0', bytes(129)]
        for index in range(8):
            values = (0, 128, 255) if index % 2 == 0 else (0, 127, 0xC8, 0xCA, 0xE0, 0xFF)
            for value in values:
                changed = bytearray(pair)
                changed[index] = value
                invalid.append(bytes(changed))
        for stream in invalid:
            with self.subTest(stream=stream.hex()):
                self.assertEqual(native(PROBE, stream)['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, pair, tempo)['status'], 0xE1)

    def test_report_validation_and_alignment(self):
        pair = fixture('short-first')
        report = native(PROBE, pair)
        for changed in (None, {}, {**report, 'playback': True}, {**report, 'status': True},
                        {**report, 'events': []}, {**report, 'end_tick': True}):
            with self.assertRaises(ValueError):
                validate(changed)
        for key, value in (('tick', 1), ('channel', 3), ('duration', 0),
                           ('opcode', 0xE0), ('half_cycle', True)):
            changed = copy.deepcopy(report)
            changed['events'][0][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        changed = copy.deepcopy(report)
        changed['events'][0]['opcode'] = 0x80
        with self.assertRaises(ValueError):
            align(changed, symbolic(pair))

    def test_reference_comparison_fails_closed(self):
        candidates = {case: native(PROBE, fixture(case)) for case in CASES}
        references = [{'model': model, 'case': case, 'pattern_interval_spc_cycles':
                       174000 if case == 'both-long' else 87000}
                      for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, references)), 6)
        for changed in (references[:-1], references[:-1]+references[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(references)
        changed[0]['pattern_interval_spc_cycles'] += 20000
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        changed = copy.deepcopy(references)
        changed[0]['pattern_interval_spc_cycles'] += 3000
        with self.assertRaises(ValueError):
            compare(candidates, changed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
