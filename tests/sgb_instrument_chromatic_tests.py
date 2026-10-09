#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned octave fixtures and fail-closed register-only reference parsing."""
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_chromatic_fixture import bank, build, NOTES
from check_sgb_instrument_chromatic_reference import observe, contract, check_models, PITCHES, SETUPS

HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def events(instrument):
    rows = [(0, 61, 0)]
    for i, pitch in enumerate(PITCHES[instrument]):
        cycle = 1000+i*87500
        for voice in (2, 3):
            for offset, value in enumerate((pitch & 255, pitch >> 8, *SETUPS[instrument]), 2):
                rows.append((cycle, 16*voice+offset, value))
        rows.append((cycle, 76, 12))
    return rows


def observed(rows, instrument=10):
    text = HEADER+''.join(f'D,0,{cycle},0,{address},{value}\n' for cycle, address, value in rows)
    return observe(io.StringIO(text), instrument)


class InstrumentChromaticTests(unittest.TestCase):
    def test_owned_transport_and_frozen_defaults(self):
        pins = {(127, 'chromatic'): '28689aa06c2e81aed0750e33648181e854082383ab29dedc958b6aa30e669c40',
                (63, 'chromatic'): 'b38f7d3bc66a89f891f1acc330b0d5e20bd740c78ae237edf1cc50b3f8fc156b',
                (127, 'calls'): '3ff6d53043426151138229fa340c75c94bee81e3d9463020541b12b2aaa94ba2',
                (63, 'calls'): 'c5bd7f878aef3f9b256ccc3a95cc5a52bfe053ffc95c2349ee78c48e61b94350'}
        for (art, case), digest in pins.items():
            self.assertEqual(hashlib.sha256(build(art, case)).hexdigest(), digest)
            baseline, changed = bank(art, case), bank(art, case, 10)
            selectors = [i+1 for i in range(len(baseline)-1) if baseline[i] == 0xE0]
            self.assertEqual(len(selectors), 2)
            expected = bytearray(baseline)
            for offset in selectors:
                expected[offset] = 10
            self.assertEqual(changed, expected)
            self.assertEqual(build(art, case, 10)[0x4004:0x4004+len(changed)], changed)
        self.assertEqual(hashlib.sha256(build(instrument=10)).hexdigest(),
                         '756cd7a43fbf660f7d0ec94ab28a35bfed3f55216e66d7ae1bd0dab73dcd7581')
        data = bank(instrument=10)
        for table in (32, 48):
            pointers = struct.unpack_from('<8H', data, table)
            self.assertEqual([i for i, pointer in enumerate(pointers) if pointer], [2, 3])
            for channel in (2, 3):
                start = pointers[channel]-0x2B00
                end = data.index(0, start)
                expected_notes = NOTES[:7] if table == 32 else NOTES[7:]
                self.assertEqual(data[end-len(expected_notes):end], bytes(0x80+n for n in expected_notes))
        for value in (True, 2.0, 0, 3, 255):
            with self.assertRaises(ValueError):
                build(instrument=value)

    def test_complete_exact_two_instrument_two_model_contract(self):
        results = []
        for instrument in SETUPS:
            result = observed(events(instrument), instrument)
            contract(result, instrument)
            self.assertEqual(len(result['notes']), 13)
            for model in ('sgb', 'sgb2'):
                results.append(dict(result, model=model, instrument=instrument, case='chromatic'))
        check_models(results)
        for changed in (results[:-1], results+[results[0]], [results[0]]*4):
            with self.assertRaises(ValueError):
                check_models(changed)
        for mutation in ('pitch', 'srcn', 'adsr1', 'adsr2', 'gain', 'voice'):
            changed = copy.deepcopy(results)
            changed[3]['notes'][6]['voices'][1][mutation] += 1
            with self.assertRaises(ValueError):
                check_models(changed)
        changed = copy.deepcopy(results)
        changed[0]['notes'][0]['mask'] = 12.0
        with self.assertRaises(ValueError):
            check_models(changed)

    def test_malformed_missing_noisy_and_extra_events_reject(self):
        rows = events(10)
        variants = [rows[1:], rows[:-1], rows+[(2000000, 76, 12)],
                    [(0, 61, 12)]+rows[1:], rows+[(0, 61, 0)],
                    rows[:1]+[(1000, 34, 256)]+rows[2:],
                    [(cycle, address, 4 if address == 76 else value) for cycle, address, value in rows],
                    [row for row in rows if row[1] != 0x37]]
        for changed in variants:
            with self.subTest(changed=changed[-1]):
                with self.assertRaises(ValueError):
                    observed(changed)
        for text in ('bad\n', HEADER+'D,0,0,0,34,bad\n', HEADER+'D,0,0,0,34\n',
                     HEADER+'D,0,0,0,34,1,extra\n', HEADER+'D,0,0,0,128,0\n',
                     HEADER+'R,0,0,0,0,0\n'*32768):
            with self.assertRaises(ValueError):
                observe(io.StringIO(text), 10)
        # Well-formed but wrong measured pitches must fail the pinned contract.
        changed = [(cycle, address, value-1 if address in (34, 50) and value else value)
                   for cycle, address, value in rows]
        with self.assertRaises(ValueError):
            contract(observed(changed), 10)

    def test_private_trace_rows_never_exported(self):
        text = HEADER+'R,0,0,0,999,PRIVATE_DIAGNOSTIC\n'
        text += ''.join(f'D,0,{cycle},0,{address},{value}\n' for cycle, address, value in events(10))
        result = observe(io.StringIO(text), 10)
        self.assertEqual(result, observed(events(10)))
        self.assertNotIn('PRIVATE', str(result))
        self.assertEqual(set(result), {'notes'})


if __name__ == '__main__':
    unittest.main()
