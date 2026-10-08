#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque duration-4 direct returns with known pending durations."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_short_return_fixture import build,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_direct_return_reference import contract as prior_contract,observe as prior_observe
from check_sgb_phrase_reference import run as phrase_run
SCHEMA='gbb-score-short-return-observation-v1'


def contract(r,case):
    prior_contract(r,case,short_return=True)


def observe(source,case,art=63,duration=8):
    return prior_observe(source,case,art,4,boundary_duration=duration)


def run(trace,firmware_dir,model,art,duration,case):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,duration,case),observer=lambda src:observe(src,case,art,duration),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000
    return r


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=16:raise ValueError('require complete short-return matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid short-return reference')
        c,a,d,m=(r.get(k) for k in ('case','return_articulation','boundary_duration','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,d,m) in seen:raise ValueError('duplicate/unknown short-return reference')
        if r.get('fixture_sha256')!=hashlib.sha256(build(a,d,c)).hexdigest():raise ValueError('short-return owned cartridge hash differs')
        seen.add((c,a,d,m))
    for c in CASES:
        for a in ARTICULATIONS:
            for d in DURATIONS:
                x,y=[r for r in refs if (r['case'],r['return_articulation'],r['boundary_duration'])==(c,a,d)]
                diffs={}
                for field in ('note_gates','onset_intervals_spc_cycles','return_control_offsets_spc_cycles','return_voice_offsets_spc_cycles','final_control_offsets_spc_cycles','boundary_pitch_to_stop_spc_cycles','unreleased_retriggers'):
                    p,q=x[field],y[field]
                    if field in ('note_gates','unreleased_retriggers'):
                        key='gate_spc_cycles' if field=='note_gates' else 'interval_spc_cycles';p,q=([v[key] for v in z] for z in (p,q))
                    diffs[field]=[abs(v-w) for v,w in zip(p,q)]
                diffs['final_stop_interval_spc_cycles']=[abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])]
                if any(v>2048 for values in diffs.values() for v in values):raise ValueError('short-return originals exceed retained allowance')
                result.append({'case':c,'return_articulation':a,'boundary_duration':d,'absolute_differences_spc_cycles':diffs})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for a in ARTICULATIONS for d in DURATIONS for m in ('sgb','sgb2')]
        result={'schema':SCHEMA,'qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
