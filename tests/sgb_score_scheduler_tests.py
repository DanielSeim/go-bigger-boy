#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference-informed scheduling, finite calls and hostile-input bounds."""
from copy import deepcopy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import schedule_sgb_score as scheduler
from schedule_sgb_score import schedule
from check_sgb_scheduler_reference import compare
import build_sgb_phrase_fixture as phrase_fixture
import build_sgb_subroutine_fixture as call_fixture


def bank(fixture, case):
    payload = fixture.score_payload(case)
    size = struct.unpack_from('<H', payload)[0]
    return payload[4:4+size]


def notes(result):
    return [event for event in result['events'] if event['kind'] == 'note']


def track_bank(stream):
    data = bytearray(36)
    struct.pack_into('<H', data, 16, 0x2B14)
    struct.pack_into('<H', data, 24, 0x2B24)
    return bytes(data)+stream


class SchedulerContracts(unittest.TestCase):
    def test_first_ending_track_and_truncation(self):
        for case, durations in phrase_fixture.CASES.items():
            result = schedule(bank(phrase_fixture, case), 0x2B10)
            boundary = min(durations)
            self.assertEqual(result['ticks'], boundary+16)
            self.assertEqual([(p['start_tick'],p['end_tick']) for p in result['patterns']],
                             [(0,boundary),(boundary,boundary+16)])
            events = notes(result)
            self.assertEqual([(e['channel'],e['tick'],e['note']) for e in events],
                             [(2,0,24),(3,0,25),(2,boundary,36),(3,boundary,36)])
            self.assertEqual([e['end_tick'] for e in events[:2]], [boundary,boundary])
            long_channels = [channel for channel,duration in zip((2,3),durations) if duration>boundary]
            self.assertEqual(result['patterns'][0]['truncated_channels'], long_channels)
            self.assertEqual([e['channel'] for e in events if e.get('truncated')],long_channels)
            self.assertEqual([e['duration'] for e in events[:2]],list(durations))

    def test_finite_call_return_and_caller_duration(self):
        for case,count in call_fixture.CASES.items():
            result = schedule(bank(call_fixture,case),0x2B10)
            events = notes(result)
            self.assertEqual([e['note'] for e in events],[24,25]*count+[36,24])
            self.assertEqual([e['tick'] for e in events],list(range(0,16*(2*count+2),16)))
            self.assertEqual(result['ticks'],16*(2*count+2))
            self.assertEqual([e['duration'] for e in events],[16]*(2*count+2))
            self.assertEqual(sum(e['kind']=='subroutine_call' for e in result['events']),1)
            self.assertEqual(sum(e['kind']=='subroutine_repeat' for e in result['events']),count-1)
            returns = [e for e in result['events'] if e['kind']=='subroutine_return']
            self.assertEqual([e['tick'] for e in returns],[32*count])
            self.assertFalse(result['qualification'])
            self.assertFalse(result['playback'])

    def test_rest_and_empty_phrase(self):
        result = schedule(track_bank(bytes((16,0x7F,0x98,0xC9,0))),0x2B10)
        timed = [e for e in result['events'] if 'duration' in e]
        self.assertEqual([(e['kind'],e['tick'],e['end_tick']) for e in timed],
                         [('note',0,16),('rest',16,32)])
        self.assertEqual(schedule(bytes(2),0x2B00)['patterns'],[])
        with self.assertRaisesRegex(ValueError,'zero-length'):
            schedule(track_bank(bytes(1)),0x2B10)

    def test_call_bounds_and_nested_or_empty_callees(self):
        data = bytearray(bank(call_fixture,'once'))
        for count in (0,4,127,255):
            changed = data.copy(); changed[49]=count
            with self.assertRaisesRegex(ValueError,'count'):
                schedule(bytes(changed),0x2B10)
        changed = data.copy(); changed[64:68]=bytes((0xEF,0x40,0x2B,1))
        with self.assertRaisesRegex(ValueError,'nested'):
            schedule(bytes(changed),0x2B10)
        changed = data.copy(); changed[64]=0
        with self.assertRaisesRegex(ValueError,'empty subroutine'):
            schedule(bytes(changed),0x2B10)
        changed = data.copy(); changed[47:49]=bytes((0,0x4B))
        with self.assertRaisesRegex(ValueError,'outside'):
            schedule(bytes(changed),0x2B10)

    def test_unsupported_channels_controls_ties_and_missing_duration(self):
        for opcode in (0xC8,0xCA,0xEA,0xF5,0xF7,0xFF):
            with self.subTest(opcode=opcode),self.assertRaises(ValueError):
                schedule(track_bank(bytes((16,0x7F,opcode,0,0,0))),0x2B10)
        for stream in (bytes((0x98,0)), bytes((16,0x98,0))):
            with self.assertRaisesRegex(ValueError,'explicit duration'):
                schedule(track_bank(stream),0x2B10)
        with self.assertRaisesRegex(ValueError,'after duration'):
            schedule(track_bank(bytes((16,0x7F,0x98,16,0x7F,0))),0x2B10)
        for channel in (0,1,3,4,5,6,7):
            changed = bytearray(track_bank(bytes((16,0x7F,0x98,0))))
            changed[24:26]=bytes(2)
            struct.pack_into('<H',changed,20+2*channel,0x2B24)
            with self.assertRaisesRegex(ValueError,'channel'):
                schedule(bytes(changed),0x2B10)
        changed=bytearray(bank(phrase_fixture,'short-first'))
        changed[94:98]=bytes((0xA4,0,0,0))
        with self.assertRaisesRegex(ValueError,'explicit duration'):
            schedule(bytes(changed),0x2B10)

    def test_ambiguous_same_tick_boundary_is_rejected(self):
        changed=bytearray(bank(phrase_fixture,'short-first'))
        changed[84]=16  # Channel 3 reaches a second note as channel 2 ends.
        changed[87]=0xA4
        with self.assertRaisesRegex(ValueError,'simultaneous'):
            schedule(bytes(changed),0x2B10)

    def test_unexecuted_long_tail_is_not_decoded(self):
        changed=bytearray(bank(phrase_fixture,'short-first'))
        changed[87]=0xFF  # Reached at tick 32, after this pattern ended at tick 16.
        result=schedule(bytes(changed),0x2B10)
        self.assertEqual(result['patterns'][0]['end_tick'],16)
        self.assertEqual(result['patterns'][0]['truncated_channels'],[3])

    def test_input_and_work_budgets(self):
        for data in (None,b'',bytes(8193),bytearray(32)):
            with self.assertRaises(ValueError):
                schedule(data,0x2B10)
        for root in (None,True,0x2AFF,0x4B00):
            with self.assertRaises(ValueError):
                schedule(bank(call_fixture,'once'),root)
        changed=bytearray(bank(call_fixture,'once'))
        changed[16:18]=bytes((1,0))
        with self.assertRaisesRegex(ValueError,'phrase controls'):
            schedule(bytes(changed),0x2B10)
        for limit,value in (('MAX_OPERATIONS',2),('MAX_EVENTS',2),('MAX_TICKS',8),('MAX_PATTERNS',1)):
            with patch.object(scheduler,limit,value),self.assertRaisesRegex(ValueError,'budget'):
                schedule(bank(phrase_fixture,'short-first'),0x2B10)

    def test_reference_alignment_rejects_old_barrier_and_wrong_repeat(self):
        oracle=schedule(bank(phrase_fixture,'short-first'),0x2B10)
        native={'keyons':[{'mask':12,'voices':[{'voice':2,'srcn':2,'pitch':1068},
                                               {'voice':3,'srcn':2,'pitch':1132}]},
                          {'mask':12,'voices':[{'voice':2,'srcn':2,'pitch':2140},
                                               {'voice':3,'srcn':2,'pitch':2140}]}],
                'pattern_interval_spc_cycles':87903}
        compare(oracle,native,'phrase')
        wrong=deepcopy(oracle)
        for event in notes(wrong)[2:]: event['tick']=32
        with self.assertRaisesRegex(ValueError,'spacing'):
            compare(wrong,native,'phrase')
        wrong_native=deepcopy(native); wrong_native['keyons'][1]['mask']=4
        with self.assertRaisesRegex(ValueError,'notes differ'):
            compare(oracle,wrong_native,'phrase')
        oracle=schedule(bank(call_fixture,'twice'),0x2B10)
        compare(oracle,{'pitches':[1068,1132]*2+[2140,1068],
                        'onset_intervals_spc_cycles':[87000]*5},'subroutine')
        with self.assertRaises(ValueError):
            compare(oracle,{'pitches':[1068,1132,2140,1068],
                            'onset_intervals_spc_cycles':[87000]*3},'subroutine')

    def test_cli_metadata_and_no_partial_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bank.bin'
            path.write_bytes(bank(call_fixture,'once'))
            command=[sys.executable,str(ROOT/'scripts/schedule_sgb_score.py'),str(path),'--phrase','0x2B10']
            result=subprocess.run(command,check=True,capture_output=True)
            report=json.loads(result.stdout)
            self.assertEqual(report['timing_domain'],'symbolic_score_ticks')
            self.assertFalse(report['qualification'])
            self.assertFalse(report['playback'])
            path.write_bytes(bytes(8193))
            result=subprocess.run(command,capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_scheduler_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')


if __name__=='__main__':
    unittest.main()
