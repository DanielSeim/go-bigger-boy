#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Variable phrase counts, log carry, transitions and prevalidation lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import sgb_score_bank_tests as banks
from build_sgb_score_list import build
from build_sgb_score_bank import build as prior_build
from build_sgb_list_fixture import bank, build as fixture_build, CASES
from check_sgb_score_list_reference import native, align, validate, compare
from check_sgb_score_polygate_reference import native_gates

# Exercise the established bank/mix/envelope contracts with the new image.
for module in (banks, banks.envelope, banks.envelope.mix):
    module.native, module.align, module.validate, module.compare = native, align, validate, compare


def authored(patterns):
    """Own phrase/table layout, independent of native expansion and caches."""
    data = bytearray(64 + 16*len(patterns))
    struct.pack_into('<H', data, 0, 0x2B10)
    for index, pair in enumerate(patterns):
        table = 64+16*index
        struct.pack_into('<H', data, 16+2*index, 0x2B00+table)
        for channel, stream in zip((2,3), pair):
            struct.pack_into('<H', data, table+2*channel, 0x2B00+len(data))
            data.extend(stream)
    return bytes(data)


class ListTests(banks.BankTests):
    def render(self, patterns):
        data = authored(patterns)
        report = native(banks.envelope.mix.PROBE, data)
        align(report, data)
        return report

    def reject(self, data):
        report = native(banks.envelope.mix.PROBE, data)
        self.assertEqual(report['status'], 0xE2)
        self.assertEqual(report['events'], [])
        self.assertEqual(report['pattern_ticks'], [])
        self.assertEqual(report['keyons'], [])
        self.assertEqual(report['pcm']['peak'], 0)

    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(), build())
        self.assertEqual(len(build()), 3131)
        self.assertEqual(hashlib.sha256(build()).hexdigest(), 'b3b3441f7d2c32b83c042373d17226f7d81d55616a56c0b4590184f411880531')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(), '1477d2c091c26d8fa49af3ec8113f411cdabe797a8311dd609278438914d3ffb')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800], prior_build()[0x1008-0x800:0x1019-0x800])
        for case in CASES:
            for art in (63,127):
                self.assertEqual(fixture_build(art,case), fixture_build(art,case))

    def test_one_to_four_patterns_and_duplicate_tables(self):
        for count in range(1,5):
            patterns = [((16,63,0x98+i,0),)*2 for i in range(count)]
            report = self.render(patterns)
            self.assertEqual(report['pattern_ticks'], list(range(0,count*16,16)))
            self.assertEqual(len(report['events']), 2*count)
            self.assertEqual(report['second_pattern_tick'], 16 if count>1 else 0)
        data = bytearray(authored([((16,63,0x98,0),)*2]*4))
        for index in range(4): struct.pack_into('<H', data, 16+2*index, 0x2B40)
        report = native(banks.envelope.mix.PROBE, bytes(data))
        align(report, bytes(data))
        self.assertEqual(report['pattern_ticks'], [0,16,32,48])

    def test_declared_end_pointer_and_word_boundaries_reject_silently(self):
        for size in (1619,2048):
            good = banks.bank(63,size=size)
            for position in (0,0xFF,0x101,0x1FF,0x201,0x3FF,0x401,0x2FF,0x4FF):
                for target in (0,0x2AFF,0x2B00+size,0x3300,0x4000,0xFFFF):
                    data = bytearray(good)
                    struct.pack_into('<H',data,position,target)
                    if position==0x101 and target==0:
                        report = native(banks.envelope.mix.PROBE,bytes(data))
                        align(report,bytes(data))
                        self.assertEqual(report['pattern_ticks'],[0])
                    else: self.reject(bytes(data))
            cuts = {1,2,3,size-1,size-2,size-8,0xFF,0x100,0x101,0x102,0x103,0x104,
                    0x1FF,0x200,0x20A,0x2FF,0x300,0x301,0x302,0x3FF,0x400,0x40A,
                    0x4FF,0x500,0x501,0x502,0x5FF,0x600,0x607}
            for cut in sorted(cuts): self.reject(good[:cut])
        self.reject(banks.bank()+b'\0')

    def test_full_64_event_log_crosses_byte_count_and_preserves_lifecycle(self):
        for art in (63,127):
            patterns = []
            for index in range(4):
                stream = (16,art,*(0x98+(index*8+note)%13 for note in range(8)),0)
                patterns.append((stream,stream))
            report = self.render(patterns)
            self.assertEqual(len(report['events']), 64)
            self.assertEqual(len(report['envelopes']), 64)
            self.assertEqual(len(report['keyons']), 32)
            self.assertEqual(report['pattern_ticks'], [0,128,256,384])
            self.assertEqual(report['end_tick'],512)
            self.assertIs(report['source_unmodified'],True)
            self.assertIs(report['cache_guards_equal'],True)

    def test_total_tick_budget_is_rehearsed_before_live_audio(self):
        rest = (127,127,0xC9,0xC9,0xC9,0xC9,0)
        report = self.render([(rest,rest)]*4)
        self.assertEqual(report['end_tick'],2032)
        self.assertEqual(report['pattern_ticks'],[0,508,1016,1524])
        self.assertEqual(report['pcm']['peak'],0)
        long_rest = (127,127,*([0xC9]*8),0)
        self.reject(authored([(long_rest,long_rest)]*4))
        # The overflow can be exactly one tick, after otherwise valid notes.
        note = (16,63,0x98,0)
        tail = (17,127,0xC9,0)
        shorter = (127,127,*([0xC9]*7),111,127,0xC9,0)
        self.reject(authored([(long_rest,long_rest),(shorter,shorter),(tail,tail)]))
        self.reject(authored([(note,note),(long_rest,long_rest),(long_rest,long_rest)]))

    def test_empty_fifth_truncated_and_late_malformed_patterns_reject(self):
        note = (16,63,0x98,0)
        self.reject(authored([]))
        self.reject(authored([(note,note)]*5))
        good = authored([(note,note)]*4)
        for cut in (17,18,19,23,24,25,len(good)-1): self.reject(good[:cut])
        for position, target in ((24,0x2B40),(0,0),(22,0x3300),(116,0),(116,0x4000)):
            data = bytearray(good)
            struct.pack_into('<H',data,position,target)
            self.reject(bytes(data))
        for bad in ((16,63,0x97,0),(17,63,0x98,0),(0xE5,160,16,63,0x98,0),
                    (0x98,0),(0xEF,0xFF,0xFF,1,0),(16,63,0x98,0xE1)):
            self.reject(authored([(note,note)]*3+[(bad,note)]))

    def test_first_end_clipping_and_ambiguous_later_boundary(self):
        short = (16,127,0xC9,0)
        held = (24,127,0x98,0)
        report = self.render([(short,held)]*4)
        self.assertEqual(report['pattern_ticks'],[0,16,32,48])
        self.assertEqual(report['end_tick'],64)
        self.assertTrue(all(edge['cause']==0 for edge in report['keyoffs']))
        note = (16,63,0x98,0)
        peer = (16,63,0x98,0x99,0)
        self.reject(authored([(note,note)]*3+[(note,peer)]))

    def test_controls_reset_per_pattern_and_song_volume_persists(self):
        reduced = (0xE5,80,16,63,0x98,0)
        note = (16,63,0x98,0)
        report = self.render([(reduced,note)]+[(note,note)]*3)
        self.assertTrue(all(edge['volumes']==[[1,1],[1,1]] for edge in report['keyons']))
        left = (0xE1,0,16,63,0x98,0)
        report = self.render([(left,left),(note,note),((0xED,64,*note),)*2,(note,note)])
        self.assertEqual([edge['volumes'] for edge in report['keyons']],
                         [[[0,11]]*2,[[7,7]]*2,[[1,1]]*2,[[7,7]]*2])

    def test_cross_page_lists_calls_and_pattern_metadata_guards(self):
        for case in CASES:
            for art in (63,127):
                data = bank(art,case)
                report = native(banks.envelope.mix.PROBE,data)
                align(report,data)
                self.assertEqual(report['pattern_ticks'],[0,96,112] if case=='three-patterns' else [0,16,96,112])
                for ticks in ([],[False,*report['pattern_ticks'][1:]],report['pattern_ticks'][:-1],
                              [0,0], [0,16,32,48,64], [0,report['end_tick']]):
                    changed = copy.deepcopy(report)
                    changed['pattern_ticks'] = ticks
                    with self.assertRaises(ValueError): align(changed,data)

    def test_three_four_pattern_reference_matrix_and_transition_guards(self):
        candidates = {f'{case}-{art}':native(banks.envelope.mix.PROBE,bank(art,case))
                      for case in CASES for art in (127,63)}
        refs = []
        for case, legacy in CASES.items():
            for art in (127,63):
                report = banks.envelope.mix.observe(io.StringIO(banks.envelope.mix.trace(legacy)),legacy)
                report['note_gates'] = [{'voice':edge['voice'],'release_kind':'keyoff',
                                        'gate_spc_cycles':int(edge['gate_spc_cycles'])}
                                       for edge in native_gates(candidates[f'{case}-{art}'])]
                refs.extend({**copy.deepcopy(report),'case':case,'articulation':art,'model':model}
                            for model in ('sgb','sgb2'))
        self.assertEqual(len(compare(candidates,refs)),8)
        for bad in (None,refs[:-1],refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError): compare(candidates,bad)
        for target in ('gate','onset','setup','model'):
            bad = copy.deepcopy(refs)
            if target=='gate': bad[0]['note_gates'][-1]['gate_spc_cycles'] += 6000
            if target=='onset': bad[0]['onset_intervals_spc_cycles'][-1] = 84000
            if target=='setup': bad[0]['keyons'][-1]['voices'][0]['volumes'][0] += 1
            if target=='model': bad[-1]['model'] = 'sgb'
            with self.assertRaises(ValueError): compare(candidates,bad)
        bad = copy.deepcopy(candidates)
        bad['four-patterns-127']['pattern_ticks'][-1] += 1
        with self.assertRaises(ValueError): compare(bad,refs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining = parser.parse_known_args()
    banks.envelope.mix.PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
