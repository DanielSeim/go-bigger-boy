#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fresh exact onset/gate comparisons for bounded score drivers against private originals."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from check_sgb_async_envelope_reference import run as original
from build_sgb_async_envelope_fixture import CASES, peer_notes

ROOT=Path(__file__).resolve().parents[1]
IMAGE_HASH='b8d12f0632cb1893ff1518fbbbeb225c0077b80b7ab1fd93f72e8d7ff331939f'
PEER_IMAGE_HASH='1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82'


def compare(native,references,cases=('clipped',),peer_gate_timing=False):
    if type(peer_gate_timing) is not bool:
        raise ValueError('invalid peer gate profile')
    image_hash=PEER_IMAGE_HASH if peer_gate_timing else IMAGE_HASH
    schema='gbb-sgb-score-peer-gate-v1' if peer_gate_timing else 'gbb-sgb-score-initial-tick-v1'
    version=0xDA if peer_gate_timing else 0xD9
    control_key='d9_control_runs' if peer_gate_timing else 'd8_control_runs'
    if not cases or len(set(cases))!=len(cases) or any(case not in CASES for case in cases):
        raise ValueError('invalid comparison cases')
    if not isinstance(native,dict) or native.get('schema')!=schema or any(
            native.get(key) is not value for key,value in (('qualification',False),('playback',False),
            ('suite_passed',True),('timing_qualified',True))) or native.get('image_sha256')!=image_hash or (
            type(native.get('reference_timing_allowance_spc_cycles')) is not int) or (
            native['reference_timing_allowance_spc_cycles']!=4096) or native.get(control_key)!=[]:
        raise ValueError('invalid native asynchronous report')
    runs=native.get('runs')
    required={(case,model,voice,slots) for case in cases for model in ('sgb','sgb2') for voice in (2,3) for slots in ((2,3),(3,2))}
    if not isinstance(runs,list) or len(runs)!=8*len(cases):
        raise ValueError('require complete native case/model/voice/slot matrix')
    seen=set()
    for row in runs:
        if not isinstance(row,dict) or row.get('case') not in cases or row.get('mode')!='native' or (
                type(row.get('held_voice')) is not int) or type(row.get('version')) is not int or (
                row['version']!=version) or not isinstance(row.get('slots'),list) or len(row['slots'])!=2 or any(
                type(slot) is not int for slot in row['slots']) or row.get('model') not in ('sgb','sgb2') or any(
                row.get(key) is not True for key in ('reset_equal','restore_equal')) or (
                type(row.get('restore_count')) is not int or not 1<=row['restore_count']<=4096):
            raise ValueError('invalid native asynchronous run metadata')
        key=(row['case'],row['model'],row['held_voice'],tuple(row['slots']))
        if key not in required or key in seen:
            raise ValueError('duplicate or unexpected native asynchronous run')
        seen.add(key)
    if not isinstance(references,list) or len(references)!=4*len(cases) or any(not isinstance(row,dict) or (
            row.get('model') not in ('sgb','sgb2')) or type(row.get('held_voice')) is not int or (
            row['held_voice'] not in (2,3)) or row.get('case') not in cases for row in references) or {
            (row['case'],row['model'],row['held_voice']) for row in references}!={(case,model,voice) for case in cases for model in ('sgb','sgb2') for voice in (2,3)}:
        raise ValueError('require complete original case/model/voice matrix')
    # Recheck the original measured envelope/setup contract, not just a timestamp.
    from check_sgb_async_envelope_reference import contract, check_models
    for row in references:
        contract(row,row['case'],row['held_voice'])
    if set(cases)==set(CASES):
        check_models(references)
    results=[]
    for row in runs:
        reference=next(ref for ref in references if (ref['case'],ref['model'],ref['held_voice'])==
                       (row['case'],row['model'],row['held_voice']))
        timings=row.get('timing')
        expected=[(voice,index) for voice in (2,3) for index in
                  range(1 if voice==row['held_voice'] else len(peer_notes(row['case'])))]
        if not isinstance(timings,list) or len(timings)!=len(expected):
            raise ValueError('incomplete native note timing')
        for (voice,index),timing,note in zip(expected,timings,reference['envelopes']):
            if not isinstance(timing,dict) or type(timing.get('voice')) is not int or timing['voice']!=voice or (
                    type(timing.get('note_index')) is not int or timing['note_index']!=index) or any(
                    timing.get(key) is not True for key in ('gate_matches','onset_matches')) or any(
                    type(timing.get(key)) not in (int,float) or not 0<=timing[key]<=400000 for key in
                    ('gate_spc_cycles','onset_spc_cycles')) or (index==0 and timing['onset_spc_cycles']!=0):
                raise ValueError('invalid native gate/onset observation')
            gate_difference=timing['gate_spc_cycles']-note['gate_spc_cycles']
            onset_difference=timing['onset_spc_cycles']-note['onset_spc_cycles']
            if abs(gate_difference)>4096 or abs(onset_difference)>4096:
                raise ValueError(f"Exact original timing exceeds allowance: {row['case']} {row['model']} "
                                 f"held={row['held_voice']} slots={row['slots']} voice={voice} note={index} "
                                 f"gate_delta={gate_difference} onset_delta={onset_difference}")
            results.append({'case':row['case'],'model':row['model'],'held_voice':row['held_voice'],'slots':row['slots'],
                            'voice':voice,'note_index':index,'native_gate_spc_cycles':timing['gate_spc_cycles'],
                            'original_gate_spc_cycles':note['gate_spc_cycles'],
                            'difference_spc_cycles':gate_difference,
                            'native_onset_spc_cycles':timing['onset_spc_cycles'],
                            'original_onset_spc_cycles':note['onset_spc_cycles'],
                            'onset_difference_spc_cycles':onset_difference})
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--all-cases',action='store_true',help='compare all four asynchronous shapes')
    parser.add_argument('--peer-gate-timing',action='store_true',help='qualify DA across all four cases')
    args=parser.parse_args()
    cases=CASES if args.all_cases or args.peer_gate_timing else ('clipped',)
    test_file='sgb_score_peer_gate_tests.py' if args.peer_gate_timing else 'sgb_score_initial_tick_tests.py'
    test_class='PeerGateTests' if args.peer_gate_timing else 'InitialTickTests'
    methods={'retrigger':'retriggers','rests':'rests','switch':'instrument_changes','clipped':'clipped_release'}
    try:
        child=subprocess.run([sys.executable,str(ROOT/'tests'/test_file),
            '--probe',str(args.probe.resolve()),
            *[test_class+'.test_'+methods[case]+'_on_each_voice_and_slot' for case in cases]],
            capture_output=True,text=True,timeout=900*len(cases))
        if child.returncode!=0 or len(child.stdout)>128*1024*len(cases):
            raise ValueError('bounded native asynchronous suite failed')
        native=json.loads(child.stdout)
        references=[dict(original(args.trace.resolve(),args.firmware_dir.resolve(),model,case,voice),held_voice=voice)
                    for case in cases for model in ('sgb','sgb2') for voice in (2,3)]
        comparisons=compare(native,references,cases,args.peer_gate_timing)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-sgb-score-peer-gate-reference-v1' if args.peer_gate_timing else 'gbb-sgb-score-initial-tick-reference-v1','qualification':False,'playback':False,
        'image_sha256':PEER_IMAGE_HASH if args.peer_gate_timing else IMAGE_HASH,'cases':list(cases),'reference_timing_allowance_spc_cycles':4096,
        'native_runs':native['runs'],'original_runs':references,'comparisons':comparisons},indent=2))


if __name__=='__main__':
    main()
