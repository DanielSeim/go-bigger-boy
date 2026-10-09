#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned asynchronous score geometry and fail-closed register observations."""
import copy
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_async_envelope_fixture import bank,build,CASES,peer_notes
from check_sgb_async_envelope_reference import observe,contract,check_models,dsp_window
from check_sgb_instrument_chromatic_reference import SETUPS,PITCHES
from check_sgb_instrument_envelope_reference import WINDOW

HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'
PINS = {
    ('retrigger',2):'c6293efa3786779856411fcad8028907c989e3634cb18f59595805effb5b3dbb',
    ('retrigger',3):'39939eab60a7fa50f0066d2e7a305e2067adbee59889269390f0fc21361da9d8',
    ('rests',2):'3f08e8bb45f40e627a8ce7b1027593699ef1e4dd12a129db836bee974afdacd2',
    ('rests',3):'ed25f0e39c42ebd7285697b7503087603fed998f5fb8bc5266035efc8800aa53',
    ('switch',2):'3f148234a5ba126b018351b0e6dafab72b4753b5cf883088c327b4c3bcdd4b5e',
    ('switch',3):'76bfbcc525953d18a895527e24096c12f7ccf26b2ca123ee6b7e3a87dde16d34',
    ('clipped',2):'d0502bd4931340729ddff25dfb3b805b293cf3487537afef9cdce220339e19e2',
    ('clipped',3):'c4fd2f5e901f45f4a422ac42324f0ed72de71dbbb9524008e279b7d260a4c470'}


def text(rows):
    return HEADER+''.join(f'{kind},0,{cycle},{sample},{address},{value}\n'
                         for cycle,sample,address,value,kind in rows)


def events(case='retrigger',held_voice=2):
    """Synthetic register stream with independently scheduled voices and silence."""
    rows = [(1000,0,0x3D,0,'D')]
    peer = 5-held_voice
    schedules = {held_voice:[(0,10,24,5400 if case=='clipped' else 10500)],peer:[]}
    for index,(instrument,note) in enumerate(peer_notes(case)):
        start = index*(5400 if case=='rests' else 2700)
        schedules[peer].append((start,instrument,note,start+2300))
    for voice in (2,3):
        for start,instrument,note,off in schedules[voice]:
            base = 16*voice
            pitch = PITCHES[instrument][note-24]
            for offset,value in enumerate((pitch&255,pitch>>8,*SETUPS[instrument]),2):
                rows.append((1000+32*start,start,base+offset,value,'D'))
            if start:
                rows.append((1000+32*start,start,0x4C,1<<voice,'D'))
            rows.append((1000+32*off,off,0x5C,1<<voice,'D'))
    rows.append((1000,0,0x4C,12,'D'))
    for sample in range(1,WINDOW+1):
        for voice in (2,3):
            start,_,_,off = next(note for note in reversed(schedules[voice]) if note[0]<sample)
            def active(offset):
                return 0 if offset<6 else min(127,(offset-5)*2) if offset<70 else max(0,127-(offset-70)//128)
            value = active(sample-start) if sample<off else max(0,active(off-start)-(sample-off)//2)
            rows.append((1000+32*sample,sample,16*voice+8,value,'E'))
    # Stable ordering preserves setup before KON and KOF before the sample at
    # that cycle. Published samples come last at shared timestamps.
    return sorted(rows,key=lambda row:(row[0],row[4]=='E'))


class AsyncEnvelopeTests(unittest.TestCase):
    def test_owned_score_geometry_and_voice_reversal(self):
        for case in CASES:
            for held in (2,3):
                data,image = bank(case,held),build(case,held)
                self.assertEqual(hashlib.sha256(image).hexdigest(),PINS[case,held])
                self.assertEqual(image,build(case,held))
                self.assertLessEqual(len(data),128)
                self.assertEqual(struct.unpack_from('<HH',image,0x4000),(len(data),0x2B00))
                self.assertEqual(image[0x4004:0x4004+len(data)],data)
                pointers = struct.unpack_from('<8H',data,32)
                self.assertEqual([i for i,p in enumerate(pointers) if p],[2,3])
                streams = {voice:data[pointers[voice]-0x2B00:(pointers[3]-0x2B00) if voice==2 else len(data)]
                           for voice in (2,3)}
                self.assertEqual(sum(stream.count(bytes((0xE7,96))) for stream in streams.values()),1)
                for voice,stream in streams.items():
                    head = bytes((0xE0,10,0xE1,10,0xED,127))
                    if voice==2:
                        head += bytes((0xE5,160,0xE7,96))
                    self.assertTrue(stream.startswith(head))
                    body = stream[len(head):]
                    if voice==held:
                        self.assertEqual(body,bytes((64,127,0x98,1,0xC9,0)))
                    elif case=='clipped':
                        self.assertEqual(body,bytes((16,127,0x98,0xC9,0)))
                    elif case=='rests':
                        self.assertEqual(body,bytes((16,127,0x98,0xC9,0x99,0xC9,1,0xC9,0)))
                    elif case=='switch':
                        self.assertEqual(body,bytes((16,127,0x98,0xE0,2,0x99,0xE0,10,0x98,0xE0,2,0x99,1,0xC9,0)))
                    else:
                        self.assertEqual(body,bytes((16,127,0x98,0x99,0x98,0x99,1,0xC9,0)))
        for case,voice in [('unknown',2),('held',2),('rests',True),('switch',3.0),('clipped',4)]:
            with self.assertRaises(ValueError):
                bank(case,voice)

    def test_exporter_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'async.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_async_envelope_fixture.py'),
                     '--case','switch','--held-voice','3','--output',str(path)]
            for expected in (0,2):
                child=subprocess.run(command,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,expected)
                self.assertEqual(path.read_bytes(),build('switch',3))

    def test_independent_onsets_releases_and_clipped_tail(self):
        for case in CASES:
            for voice in (2,3):
                result = observe(io.StringIO(text(events(case,voice))),case,voice)
                self.assertEqual(result['sample_count_per_voice'],12000)
                self.assertEqual(result['held_setup_writes'],0)
                held = next(row for row in result['envelopes'] if row['voice']==voice)
                self.assertEqual(held['gate_spc_cycles'],32*(5400 if case=='clipped' else 10500))
                self.assertEqual(len(result['envelopes']),1+len(peer_notes(case)))
                self.assertTrue(all(row['release_zero'] for row in result['envelopes']))
                self.assertNotIn('samples',result)

    def test_missing_samples_setup_key_edges_and_release_are_rejected(self):
        baseline = events()
        variants = [baseline[:-1], [row for row in baseline if not(row[4]=='E' and row[1]==100)],
                    [row for row in baseline if row[2]!=0x5C],
                    [(c,s,a,4 if a==0x4C and s==0 else v,k) for c,s,a,v,k in baseline],
                    [(c,s,a,11 if a==0x34 and s>0 else v,k) for c,s,a,v,k in baseline],
                    [(c,s,a,128 if k=='E' else v,k) for c,s,a,v,k in baseline],
                    [(c,s,a,1 if k=='E' and s>11500 else v,k) for c,s,a,v,k in baseline],
                    [(c+1 if k=='E' and s==100 else c,s,a,v,k) for c,s,a,v,k in baseline]]
        for index,rows in enumerate(variants):
            with self.subTest(case=index), self.assertRaises(ValueError):
                observe(io.StringIO(text(rows)),'retrigger',2)
        for trace in ('bad\n',HEADER+'D,0,0,0,61,bad\n',HEADER+'D,0,0,0,61\n',
                      HEADER+'D,0,0,0,61,0,extra\n',HEADER+'R,0,0,0,0,0\n'*32768):
            with self.assertRaises(ValueError):
                observe(io.StringIO(trace),'retrigger',2)

    def test_held_register_changes_are_observed_and_private_rows_discarded(self):
        baseline = events('rests',3)
        original = observe(io.StringIO(text(baseline)),'rests',3)
        changed = sorted(baseline+[(4200,100,0x35,142,'D')],key=lambda row:row[0])
        self.assertEqual(observe(io.StringIO(text(changed)),'rests',3)['held_setup_writes'],1)
        private = text(baseline).replace(HEADER,HEADER+'R,0,0,0,999,PRIVATE_DIAGNOSTIC\n')
        self.assertEqual(observe(io.StringIO(private),'rests',3),original)

    def test_pinned_clipped_measurement_and_mutation_guards(self):
        # Fresh SGB1 register-only observation. No private score/sample/PCM data.
        held = {'voice':2,'instrument':10,'base_note':24,'pitch':7993,'srcn':10,
                'adsr1':142,'adsr2':175,'gain':184,'onset_spc_cycles':0,
                'gate_spc_cycles':170900,'peak':127,'peak_sample_offset':197,
                'active_last':92,'active_points':dict(zip(map(str,(1,4,8,16,32,64,128,512,2048,4096)),
                                                        (0,0,2,6,18,38,82,125,113,99))),
                'release_start':92,'release_drops':92,'release_drop_samples':2,
                'release_zero':True,'release_zero_spc_cycles':5886}
        peer = dict(held,voice=3,gate_spc_cycles=72953,active_last=111,release_start=111,
                    release_drops=111,release_zero_spc_cycles=7161,
                    active_points={key:value for key,value in held['active_points'].items() if key!='4096'})
        result = {'held_setup_writes':0,'sample_count_per_voice':12000,'envelopes':[held,peer]}
        contract(result,'clipped',2)
        for key,value in (('voice',3),('instrument',2),('base_note',25),('pitch',7992),('srcn',2),
                ('adsr1',143),('adsr2',111),('gain',185),('onset_spc_cycles',1),('gate_spc_cycles',334000),
                ('peak',126),('peak',127.0),('peak_sample_offset',198),('active_last',91),
                ('release_start',91),('release_drops',91),('release_drop_samples',1),('release_zero',1),
                ('release_zero_spc_cycles',5885),('release_zero_spc_cycles',6017),('active_points',{})):
            changed = copy.deepcopy(result)
            changed['envelopes'][0][key]=value
            with self.subTest(key=key,value=value), self.assertRaises(ValueError):
                contract(changed,'clipped',2)
        for changed in (dict(result,held_setup_writes=1),dict(result,held_setup_writes=False),
                        dict(result,sample_count_per_voice=12000.0),dict(result,envelopes=[held]),
                        dict(result,envelopes=[peer,held])):
            with self.assertRaises(ValueError):
                contract(changed,'clipped',2)
        for reports in ([],[dict(result,model='sgb',case='clipped',held_voice=2)]*16):
            with self.assertRaises(ValueError):
                check_models(reports)

    def test_asynchronous_sampler_witness_excludes_envx_and_checks_completeness(self):
        baseline = events('switch')
        sampled = dsp_window(io.StringIO(text(baseline)))
        plain = [row for row in baseline if row[4]=='D']
        plain.append((1000+32*12001,12001,0x5C,0,'D'))
        self.assertEqual(dsp_window(io.StringIO(text(plain))),sampled)
        changed = [(c,s,a,2 if a==0x34 and s==0 else v,k) for c,s,a,v,k in plain]
        self.assertNotEqual(dsp_window(io.StringIO(text(changed))),sampled)
        for rows in (baseline[:-2],[row for row in baseline if not(row[2]==0x4C and row[1]>0)]):
            with self.assertRaises(ValueError):
                dsp_window(io.StringIO(text(rows)))


if __name__ == '__main__':
    unittest.main()
