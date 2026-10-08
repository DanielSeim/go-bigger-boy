#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque reference checks for per-channel duration/articulation inheritance."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from check_sgb_score_reselect_reference import validate as reselect_validate,SCHEMA as RESELECT_SCHEMA
from check_sgb_score_polygate_reference import align as gate_align
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_mix_reference import selected
from schedule_sgb_score import schedule
from build_sgb_score_timing_fixture import bank,build as fixture_build,expected,CASES
from check_sgb_score_sparse_reference import onsets,onset_contract
from check_sgb_score_polygate_reference import observe as gate_observe,native as gate_native,native_gates
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer



SCHEMA='gbb-spc-score-timing-v1'


def validate(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA:raise ValueError('invalid mix inheritance schema')
    reselect_validate({**report,'schema':RESELECT_SCHEMA})


def align(report,data):
    validate(report)
    gate_align(report,data,schema=SCHEMA,max_events=64,max_ticks=2032,pitch_table=PITCH,
               isolated_voices=False,pattern_ticks=report['pattern_ticks'],sparse=True,inherit_timing=True)
    symbolic=schedule(data,int.from_bytes(data[:2],'little'),inherit_timing=True)
    if report['pattern_masks']!=[sum(1<<channel for channel in pattern['channels']) for pattern in symbolic['patterns']]:
        raise ValueError('inheritance pattern masks differ')
    controls={2:[10,127],3:[10,127]};counts={2:0,3:0};song=160;index=0
    for event in symbolic['events']:
        channel,kind=event['channel'],event['kind']
        if kind=='pan':controls[channel][0]=event['value']
        elif kind=='track_volume':controls[channel][1]=event['value']
        elif kind=='song_volume':song=event['value']
        elif kind=='instrument':counts[channel]+=1
        elif kind in ('note','rest'):
            actual=report['events'][index];index+=1
            pan,track=controls[channel]
            if [actual['pan'],actual['track_volume'],actual['song_volume'],actual['volumes'],actual['instrument_sets']]!=[pan,track,song,selected(pan,track,song,kind=='rest'),counts[channel]]:
                raise ValueError('native inheritance differs from executed symbolic controls')
            counts[channel]=0
    if index!=len(report['events']):raise ValueError('inheritance event count differs')


def native(probe,data,tempo=96):
    from build_sgb_score_timing import build
    return gate_native(probe,data,tempo,builder=build,validator=validate,output_bound=131072)


def compare(candidates,references):
    keys={f'{case}-{art}' for case in CASES for art in (63,127)}
    if not isinstance(candidates,dict) or set(candidates)!=keys or not isinstance(references,list) or len(references)!=8:
        raise ValueError('require inheritance cases/articulations/models')
    seen=set();comparisons=[]
    for reference in references:
        if not isinstance(reference,dict):raise ValueError('invalid inheritance reference')
        case,art,model=(reference.get(key) for key in ('case','articulation','model'))
        if case not in CASES or type(art) is not int or art not in (63,127) or model not in ('sgb','sgb2') or (case,art,model) in seen:
            raise ValueError('invalid/duplicate inheritance reference')
        seen.add((case,art,model));contract(reference,case)
        report=candidates[f'{case}-{art}'];align(report,bank(art,case))
        if len(report['keyons'])!=8:raise ValueError('inheritance native onset count differs')
        for edge,wanted in zip(report['keyons'],expected(case)):
            if edge['mask']!=wanted['mask'] or any(edge['pitches'][voice['voice']-2]!=voice['pitch'] or edge['volumes'][voice['voice']-2]!=voice['volumes'] for voice in wanted['voices']):
                raise ValueError('inheritance native onset differs')
        actual=native_gates(report);original=reference['note_gates']
        differences=[abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(actual,original)]
        if len(actual)!=12 or any(a['voice']!=b['voice'] or a['cause']!=1 for a,b in zip(actual,original)) or any(value>4096 for value in differences):
            raise ValueError('inheritance gate outside existing allowance')
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if len(intervals)!=7 or any(abs(a-b)>4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):
            raise ValueError('inheritance onset outside existing allowance')
        comparisons.append({'case':case,'articulation':art,'model':model,'absolute_gate_difference_spc_cycles':differences,
                            'native_DSP_intervals_spc_cycles':intervals,'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles']})
    for case in CASES:
        for art in (63,127):
            a,b=[r for r in references if r['case']==case and r['articulation']==art]
            if a['keyons']!=b['keyons'] or any(abs(x-y)>2048 for x,y in zip(a['onset_intervals_spc_cycles'],b['onset_intervals_spc_cycles'])):
                raise ValueError('inheritance original models disagree')
    return comparisons

def contract(result,case):
    onset_contract(result,case,expected_onsets=expected(case))
    voices=[voice['voice'] for edge in expected(case) for voice in edge['voices']]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(voices) or any(not isinstance(gate,dict) or type(gate.get('voice')) is not int or gate['voice']!=voice or gate.get('release_kind')!='keyoff' or not integer(gate.get('gate_spc_cycles'),1,200000) for gate,voice in zip(gates,voices)):
        raise ValueError('inheritance requires directly observed releases')


def observe(source,case):
    result=gate_observe(source,onset_observer=lambda trace:onsets(trace,case,expected_onsets=expected(case)),expected_notes=12,handoff_note_count=None)
    contract(result,case)
    return result


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:fixture_build(art,name),observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--probe',type=Path)
    args=parser.parse_args()
    try:
        references=[run(args.trace.resolve(),args.firmware_dir.resolve(),model,art,case) for case in CASES for art in (127,63) for model in ('sgb','sgb2')]
        result={'schema':'gbb-score-timing-reference-v1','qualification':False,'playback':False,'reference':references}
        if args.probe:
            from build_sgb_score_timing import build
            candidates={f'{case}-{art}':native(args.probe.resolve(),bank(art,case)) for case in CASES for art in (127,63)}
            result.update(program_sha256=hashlib.sha256(build()).hexdigest(),native=candidates,comparisons=compare(candidates,references))
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
