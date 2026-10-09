#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned envelope fixture transport and fail-closed ENVX summary parsing."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_instrument_envelope_fixture import bank,build,CASES
from check_sgb_instrument_envelope_reference import observe,contract,check_models,dsp_window,WINDOW

TRACE = None
HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def rows():
    events = [(1000,0,61,0,'D')]
    for voice in (2,3):
        for offset,value in enumerate((0x39,0x1F,10,0x8E,0xAF,0xB8),2):
            events.append((1000,0,16*voice+offset,value,'D'))
    events.append((1000,0,76,12,'D'))
    for sample in range(1,WINDOW+1):
        cycle = 1000+32*sample
        if sample == 9000:
            events.append((cycle, sample,92,12,'D'))
        if sample < 6:
            value = 0
        elif sample < 64:
            value = (sample-5)*2
        elif sample < 9000:
            value = 127-(sample-64)//128
        else:
            value = max(0,58-(sample-9000)//2)
        for voice in (2,3):
            events.append((cycle,sample,16*voice+8,value,'E'))
    return events


def text(events):
    return HEADER+''.join(f'{kind},0,{cycle},{sample},{address},{value}\n'
                          for cycle,sample,address,value,kind in events)


class EnvelopeTests(unittest.TestCase):
    def test_owned_fixture_channels_controls_and_bounds(self):
        pins = {'held':'7838e5982fb2f7fe1089b2994ca6c7f60ced80614de66ef5ccdf8831e7c8b11b',
                'short':'973758a4dbc0f8bf0753579b9612480052581bc37d1533cc1d9f0bac38af92d9',
                'retrigger':'a79a6b692bf16be78bd6727f20cdbde3f8230486b1ff301ca6b726745c129319'}
        for case,digest in pins.items():
            self.assertEqual(hashlib.sha256(build(10,case)).hexdigest(),digest)
        for instrument in (2,10):
            for case,(duration,art,notes) in CASES.items():
                data,image = bank(instrument,case),build(instrument,case)
                self.assertLessEqual(len(data),128)
                self.assertEqual(image,build(instrument,case))
                self.assertEqual(image[0x4004:0x4004+len(data)],data)
                self.assertEqual(struct.unpack_from('<HH',image,0x4000),(len(data),0x2B00))
                pointers = struct.unpack_from('<8H',data,32)
                self.assertEqual([i for i,p in enumerate(pointers) if p],[2,3])
                for voice in (2,3):
                    start = pointers[voice]-0x2B00
                    stream = bytes((0xE0,instrument,0xE1,10,0xED,127))
                    if voice == 2:
                        stream += bytes((0xE5,160,0xE7,96))
                    stream += bytes((duration,art,*(0x80+n for n in notes),1,0xC9,0))
                    self.assertEqual(data[start:start+len(stream)],stream)
        for instrument in (True,10.0,0,255):
            with self.assertRaises(ValueError):
                bank(instrument)
        with self.assertRaises(ValueError):
            bank(case='unknown')

    def test_bounded_published_register_summary(self):
        result = observe(io.StringIO(text(rows())),10,'held')
        self.assertEqual(result['sample_count_per_voice'],12000)
        self.assertEqual(len(result['envelopes']),2)
        for env in result['envelopes']:
            self.assertEqual(env['peak'],127)
            self.assertEqual(env['peak_sample_offset'],64)
            self.assertTrue(env['release_zero'])
            self.assertEqual(env['release_drop_samples'],2)
        self.assertNotIn('samples',result)
        self.assertNotIn('ram',str(result))

    def test_incomplete_noisy_changed_setup_and_invalid_windows(self):
        baseline = rows()
        variants = [baseline[:-1],baseline[1:],baseline[:14]+baseline[16:],
                    baseline+[(999999,WINDOW+1,40,0,'E')],
                    [(c,s,a,127 if a == 0x25 else v,k) for c,s,a,v,k in baseline],
                    [(c,s,a,12 if a == 61 else v,k) for c,s,a,v,k in baseline],
                    [(c,s,a,4 if a == 76 else v,k) for c,s,a,v,k in baseline],
                    [event for event in baseline if event[2] != 92],
                    [(c,s,a,128 if k == 'E' else v,k) for c,s,a,v,k in baseline],
                    [(c,s,a,v,k) for c,s,a,v,k in baseline if not(k == 'E' and s == 200)],
                    [(c+(1 if k == 'E' and s == 200 else 0),s,a,v,k) for c,s,a,v,k in baseline]]
        for i,events in enumerate(variants):
            with self.subTest(case=i):
                with self.assertRaises(ValueError):
                    observe(io.StringIO(text(events)),10,'held')
        for trace in ('bad\n',HEADER+'E,0,0,0,40,bad\n',HEADER+'D,0,0,0,61\n',
                      HEADER+'D,0,0,0,61,0,extra\n',HEADER+'R,0,0,0,0,0\n'*32768):
            with self.assertRaises(ValueError):
                observe(io.StringIO(trace),10,'held')

    def test_private_diagnostics_not_retained(self):
        baseline = text(rows())
        changed = baseline.replace(HEADER,HEADER+'R,0,0,0,999,PRIVATE_DIAGNOSTIC\n')
        self.assertEqual(observe(io.StringIO(baseline),10,'held'),observe(io.StringIO(changed),10,'held'))

    def test_sampler_witness_compares_only_bounded_dsp_writes(self):
        events = rows()
        sampled = dsp_window(io.StringIO(text(events)))
        plain = [row for row in events if row[4] == 'D']
        plain.append((1000+32*12001,12001,92,0,'D'))
        self.assertEqual(dsp_window(io.StringIO(text(plain))),sampled)
        changed = [(c,s,a,11 if a == 0x24 else v,k) for c,s,a,v,k in plain]
        self.assertNotEqual(dsp_window(io.StringIO(text(changed))),sampled)
        with self.assertRaises(ValueError):
            dsp_window(io.StringIO(text(events[:-2])))
        with self.assertRaises(ValueError):
            dsp_window(io.StringIO(HEADER+'D,0,0,0,76,4\n'))

    def test_pinned_measured_summary_and_mutation_guards(self):
        # Sanitized first held-note observations, independently recorded from
        # both original models. No private trace, sample or RAM data.
        points = dict(zip(map(str,(1,4,8,16,32,64,128,512,2048,4096,8192)),
                          (0,0,2,6,18,38,82,125,113,99,79)))
        setup = [{'voice':voice,'pitch':7993,'srcn':10,'adsr1':142,'adsr2':175,'gain':184}
                 for voice in (2,3)]
        envelope = {'voice':2,'base_note':24,'gate_spc_cycles':334957,'peak':127,
                    'peak_sample_offset':197,'active_last':70,'active_points':points,
                    'release_start':70,'release_drops':70,'release_drop_samples':2,
                    'release_zero':True,'release_zero_spc_cycles':4521}
        peer = dict(envelope,voice=3,gate_spc_cycles=335212,release_zero_spc_cycles=4522)
        result = {'setup':setup,'envelopes':[envelope,peer],'sample_count_per_voice':12000}
        contract(result,10,'held')
        for key,value in (('voice',3),('base_note',25),('gate_spc_cycles',300000),
                ('peak',126),('peak_sample_offset',196),('active_last',71),
                ('release_start',69),('release_drops',69),('release_drop_samples',1),
                ('release_zero',False),('release_zero',1),('release_zero_spc_cycles',9000),
                ('peak',127.0),('active_points',{}),('active_points',dict(points,**{'8':3}))):
            changed = copy.deepcopy(result)
            changed['envelopes'][0][key] = value
            with self.subTest(key=key,value=value):
                with self.assertRaises(ValueError):
                    contract(changed,10,'held')
        for changed in ({**result,'sample_count_per_voice':12000.0},
                        {**result,'envelopes':result['envelopes'][:1]},
                        {**result,'setup':[]}):
            with self.assertRaises(ValueError):
                contract(changed,10,'held')
        for reports in ([],[dict(result,model='sgb',instrument=10,case='held')]*12):
            with self.assertRaises(ValueError):
                check_models(reports)

    def test_trace_cli_rejects_unbounded_configurations(self):
        if TRACE is None:
            self.skipTest('trace executable required')
        base = [str(TRACE),'none','none','--sync-gb-sgb1','none','none']
        for flags in (['--voice-envelope-trace'],['--voice-envelope-trace','--fractional-apu-sync'],
                      ['--voice-envelope-trace','--sound-event-trace-output','/tmp/unused-env.csv']):
            child = subprocess.run(base+flags,capture_output=True,text=True,timeout=10)
            self.assertEqual(child.returncode,2)
            self.assertIn('voice envelope trace requires',child.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--trace',type=Path)
    args,remaining = parser.parse_known_args()
    TRACE = args.trace.resolve() if args.trace else None
    unittest.main(argv=[sys.argv[0],*remaining])
