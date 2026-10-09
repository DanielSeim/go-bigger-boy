#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fresh exact clipped-gate comparisons for D9 against private originals."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from check_sgb_async_envelope_reference import run as original

ROOT=Path(__file__).resolve().parents[1]
IMAGE_HASH='b8d12f0632cb1893ff1518fbbbeb225c0077b80b7ab1fd93f72e8d7ff331939f'


def compare(native,references):
    if not isinstance(native,dict) or native.get('schema')!='gbb-sgb-score-initial-tick-v1' or any(
            native.get(key) is not value for key,value in (('qualification',False),('playback',False),
            ('suite_passed',True),('timing_qualified',True))) or native.get('image_sha256')!=IMAGE_HASH or (
            type(native.get('reference_timing_allowance_spc_cycles')) is not int) or (
            native['reference_timing_allowance_spc_cycles']!=4096) or native.get('d8_control_runs')!=[]:
        raise ValueError('invalid D9 native clipped report')
    runs=native.get('runs')
    required={(model,voice,slots) for model in ('sgb','sgb2') for voice in (2,3) for slots in ((2,3),(3,2))}
    if not isinstance(runs,list) or len(runs)!=8:
        raise ValueError('require all eight D9 clipped model/voice/slot runs')
    seen=set()
    for row in runs:
        if not isinstance(row,dict) or row.get('case')!='clipped' or row.get('mode')!='native' or (
                type(row.get('held_voice')) is not int) or type(row.get('version')) is not int or (
                row['version']!=0xD9) or not isinstance(row.get('slots'),list) or len(row['slots'])!=2 or any(
                type(slot) is not int for slot in row['slots']) or row.get('model') not in ('sgb','sgb2') or any(
                row.get(key) is not True for key in ('reset_equal','restore_equal')) or (
                type(row.get('restore_count')) is not int or not 1<=row['restore_count']<=4096):
            raise ValueError('invalid D9 clipped run metadata')
        key=(row['model'],row['held_voice'],tuple(row['slots']))
        if key not in required or key in seen:
            raise ValueError('duplicate or unexpected D9 clipped run')
        seen.add(key)
    if not isinstance(references,list) or len(references)!=4 or any(not isinstance(row,dict) or (
            row.get('model') not in ('sgb','sgb2')) or type(row.get('held_voice')) is not int or (
            row['held_voice'] not in (2,3)) or row.get('case')!='clipped' for row in references) or {
            (row['model'],row['held_voice']) for row in references}!={(model,voice) for model in ('sgb','sgb2') for voice in (2,3)}:
        raise ValueError('require four original clipped runs')
    # Recheck the original measured envelope/setup contract, not just a timestamp.
    from check_sgb_async_envelope_reference import contract
    for row in references:
        contract(row,'clipped',row['held_voice'])
    results=[]
    for row in runs:
        reference=next(ref for ref in references if (ref['model'],ref['held_voice'])==(row['model'],row['held_voice']))
        timings=row.get('timing')
        if not isinstance(timings,list) or len(timings)!=2:
            raise ValueError('incomplete native clipped note timing')
        for voice,timing in zip((2,3),timings):
            if not isinstance(timing,dict) or type(timing.get('voice')) is not int or timing['voice']!=voice or (
                    type(timing.get('note_index')) is not int or timing['note_index']!=0) or any(
                    timing.get(key) is not True for key in ('gate_matches','onset_matches')) or any(
                    type(timing.get(key)) not in (int,float) or not 0<=timing[key]<=400000 for key in
                    ('gate_spc_cycles','onset_spc_cycles')) or timing['onset_spc_cycles']!=0:
                raise ValueError('invalid native clipped gate observation')
            note=next(note for note in reference['envelopes'] if note['voice']==voice)
            difference=timing['gate_spc_cycles']-note['gate_spc_cycles']
            if abs(difference)>4096:
                raise ValueError('D9 exact original gate difference exceeds retained allowance')
            results.append({'model':row['model'],'held_voice':row['held_voice'],'slots':row['slots'],
                            'voice':voice,'native_gate_spc_cycles':timing['gate_spc_cycles'],
                            'original_gate_spc_cycles':note['gate_spc_cycles'],
                            'difference_spc_cycles':difference})
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        child=subprocess.run([sys.executable,str(ROOT/'tests/sgb_score_initial_tick_tests.py'),
            '--probe',str(args.probe.resolve()),'InitialTickTests.test_clipped_release_on_each_voice_and_slot'],
            capture_output=True,text=True,timeout=900)
        if child.returncode!=0 or len(child.stdout)>128*1024:
            raise ValueError('bounded native D9 clipped suite failed')
        native=json.loads(child.stdout)
        references=[dict(original(args.trace.resolve(),args.firmware_dir.resolve(),model,'clipped',voice),held_voice=voice)
                    for model in ('sgb','sgb2') for voice in (2,3)]
        comparisons=compare(native,references)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-sgb-score-initial-tick-reference-v1','qualification':False,'playback':False,
        'image_sha256':IMAGE_HASH,'reference_timing_allowance_spc_cycles':4096,
        'native_runs':native['runs'],'original_runs':references,'comparisons':comparisons},indent=2))


if __name__=='__main__':
    main()
