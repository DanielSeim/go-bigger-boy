#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded multi-page source ownership, pointers, calls and full lifecycle."""
import argparse
import copy
import hashlib
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import sgb_score_envelope_tests as envelope
from build_sgb_score_bank import build, MAX_BANK
from build_sgb_score_envelope import build as prior_build
from build_sgb_bank_fixture import bank, CASES
from check_sgb_score_bank_reference import native, align, validate, compare
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_gate_reference import PULSES

# Re-run the established mix/envelope contracts through the relocated caches.
envelope.native, envelope.align, envelope.validate, envelope.compare = native, align, validate, compare
envelope.mix.native, envelope.mix.align, envelope.mix.validate, envelope.mix.compare = native, align, validate, compare


class BankTests(envelope.EnvelopeTests):
    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'1477d2c091c26d8fa49af3ec8113f411cdabe797a8311dd609278438914d3ffb')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'4cf02fde17d9af55a3583fc5d643fe09cee04b0828694fbd1c630a092abb8e1c')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_page_crossing_banks_calls_repeats_returns_and_last_byte(self):
        for size in (1619,MAX_BANK):
            for case in CASES:
                for art in (63,127):
                    data = bank(art,case,size)
                    report = native(envelope.mix.PROBE,data)
                    align(report,data)
                    self.assertEqual(report['end_tick'],128)
                    self.assertEqual(len(report['envelopes']),16)
                    self.assertIs(report['source_unmodified'],True)
                    self.assertIs(report['cache_guards_equal'],True)
                    self.assertEqual(data[-1],0)
        # A same-low-byte wrong high byte must select different source bytes,
        # not silently alias the valid return/callee/phrase/table page.
        good = bank(63)
        for position in (0,0xFF,0x1FF,0x3FF,0x2FF,0x4FF):
            changed = bytearray(good)
            target = int.from_bytes(changed[position:position+2],'little')
            struct.pack_into('<H',changed,position,target-0x100)
            self.assertEqual(native(envelope.mix.PROBE,bytes(changed))['status'],0xE2)

    def test_page_crossing_with_every_gate_profile(self):
        for tempo,art,duration in PULSES:
            data = bytearray(bank(art))
            # Four duration/articulation pairs are in caller/pattern streams;
            # the shared body inherits through each repeat and return.
            for offset in (0x2F1+10,0x4F5+6,0x6FD+4,MAX_BANK-9+4):
                data[offset] = duration
            data[0x2F1+9] = tempo
            report = native(envelope.mix.PROBE,bytes(data),tempo)
            align(report,bytes(data))
            self.assertEqual(report['end_tick'],8*duration)

    def test_declared_end_pointer_and_word_boundaries_reject_silently(self):
        for size in (1619,MAX_BANK):
            good = bank(63,size=size)
            for position in (0,0xFF,0x101,0x1FF,0x201,0x3FF,0x401,0x2FF,0x4FF):
                for target in (0,0x2AFF,0x2B00+size,0x3300,0x4000,0xFFFF):
                    data = bytearray(good)
                    struct.pack_into('<H',data,position,target)
                    self.assertEqual(native(envelope.mix.PROBE,bytes(data))['status'],0xE2)
            cuts = {1,2,3,size-1,size-2,size-8,0xFF,0x100,0x101,0x102,0x103,0x104,
                    0x1FF,0x200,0x20A,0x2FF,0x300,0x301,0x302,0x3FF,0x400,0x40A,
                    0x4FF,0x500,0x501,0x502,0x5FF,0x600,0x607}
            for cut in sorted(cuts):
                self.assertEqual(native(envelope.mix.PROBE,good[:cut])['status'],0xE2)
        self.assertEqual(native(envelope.mix.PROBE,bank()+b'\0')['status'],0xE2)

    def test_word_and_control_operand_at_exclusive_end(self):
        good = bank(63)
        for tail in ((0xEF,0xFF),(0xEF,0xFF,0x30),(0xE1,), (16,), (16,63)):
            # A valid pointer can still leave a two-byte operand or complete
            # timed event unavailable; no read past the declared bank is legal.
            data = bytearray(good)
            start = MAX_BANK-len(tail)
            struct.pack_into('<H',data,0x3FF,0x2B00+start)
            data[start:] = bytes(tail)
            self.assertEqual(native(envelope.mix.PROBE,bytes(data))['status'],0xE2)

    def test_old_cache_layout_and_false_preservation_reports_fail(self):
        report = native(envelope.mix.PROBE,bank(63))
        for key in ('source_unmodified','cache_guards_equal'):
            changed = copy.deepcopy(report)
            changed[key] = False
            with self.assertRaises(ValueError): validate(changed)
        # The old image writes inside the reserved source pool, even with a
        # compact input. The real component guard must detect that mutation.
        with self.assertRaises(ValueError):
            gate_native(envelope.mix.PROBE,envelope.mix.bank(63),builder=prior_build,validator=validate)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining = parser.parse_known_args()
    envelope.mix.PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
