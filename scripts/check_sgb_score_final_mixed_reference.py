#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque observations of held-127 voices returning through 63 rests."""
import argparse,json,subprocess
from pathlib import Path
from build_sgb_score_final_mixed_fixture import build,CASES,RESTS,DURATIONS
from check_sgb_score_final_return_reference import contract as prior_contract,observe as prior_observe
from check_sgb_phrase_reference import run as phrase_run
SCHEMA='gbb-score-final-mixed-observation-v1'


def contract(r,case):
    prior_contract(r,case,return_art=63)


def run(trace,firmware_dir,model,duration,case,rest):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(127,duration,case,rest),observer=lambda src:prior_observe(src,case,127,duration,rest,return_art=63),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000
    return r


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=16:raise ValueError('require complete mixed-articulation matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid mixed reference')
        c,d,s,m=(r.get(k) for k in ('case','boundary_duration','return_rest','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,d,s,m) in seen:raise ValueError('duplicate/unknown mixed reference')
        seen.add((c,d,s,m))
    for c in CASES:
        for s in RESTS:
            for d in DURATIONS:
                x,y=[r for r in refs if (r['case'],r['boundary_duration'],r['return_rest'])==(c,d,s)]
                diffs={}
                for field in ('note_gates','onset_intervals_spc_cycles','final_control_offsets_spc_cycles','boundary_pitch_to_stop_spc_cycles'):
                    a,b=x[field],y[field]
                    if field=='note_gates':a,b=([v['gate_spc_cycles'] for v in z] for z in (a,b))
                    diffs[field]=[abs(p-q) for p,q in zip(a,b)]
                diffs['final_stop_interval_spc_cycles']=[abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])]
                if any(v>2048 for vs in diffs.values() for v in vs):raise ValueError('mixed originals exceed retained allowance')
                result.append({'case':c,'return_rest':s,'boundary_duration':d,'absolute_differences_spc_cycles':diffs})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);a=p.parse_args()
    try:
        refs=[run(a.trace.resolve(),a.firmware_dir.resolve(),m,d,c,s) for c in CASES for s in RESTS for d in DURATIONS for m in ('sgb','sgb2')]
        result={'schema':SCHEMA,'qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
