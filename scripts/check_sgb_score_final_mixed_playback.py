#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate measured articulation-63 returning rests and pending final audio."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_final_mixed import build as program_build
from build_sgb_score_final_mixed_fixture import bank,build,CASES,RESTS,DURATIONS
from check_sgb_score_final_mixed_reference import run,agree,SCHEMA as REF_SCHEMA
from check_sgb_score_final_return_playback import validate as prior_validate,identity as prior_identity,SCHEMA as PRIOR_SCHEMA,observation,control_writes
from check_sgb_score_polygate_reference import native as gate_native
SCHEMA='gbb-spc-score-final-mixed-v1'


def identity(r):return prior_identity(r,return_art=63)


def validate(r):
    if r.get('schema')!=SCHEMA:raise ValueError('invalid mixed playback schema')
    prior_validate({**r,'schema':PRIOR_SCHEMA},return_art=63,fixture_bank=bank)


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,refs):
    agree(refs)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{s}-{d}' for c in CASES for s in RESTS for d in DURATIONS}:raise ValueError('require complete mixed native matrix')
    result=[]
    for ref in refs:
        c,d,s,m=(ref[k] for k in ('case','boundary_duration','return_rest','model'));r=candidates[f'{c}-{s}-{d}'];validate(r)
        if identity(r)!=(c,127,d,s):raise ValueError('mixed native matrix identity differs')
        if ref.get('fixture_sha256')!=hashlib.sha256(build(127,d,c,s)).hexdigest():raise ValueError('mixed owned cartridge hash differs')
        gates,held=observation(r);controls=control_writes(r);stop=controls[0]['half_cycle']
        gd=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,ref['note_gates'])]
        od=[abs((y['half_cycle']-x['half_cycle'])/2-o) for x,y,o in zip(r['keyons'],r['keyons'][1:],ref['onset_intervals_spc_cycles'])]
        sd=abs((stop-r['keyons'][3]['half_cycle'])/2-ref['final_stop_interval_spc_cycles'])
        pd=[abs(x-y) for x,y in zip([(stop-w['half_cycle'])/2 for w in r['voice_writes'] if w['address']==35 and w['half_cycle']>r['keyons'][3]['half_cycle']],ref['boundary_pitch_to_stop_spc_cycles'])]
        cd=[abs((w['half_cycle']-stop)/2-o) for w,o in zip(controls,ref['final_control_offsets_spc_cycles'])]
        if len(pd)!=len(ref['boundary_pitch_writes']) or held!=ref['unreleased_final_voices'] or any(v>4096 for v in gd+od+pd+cd+[sd]):raise ValueError('mixed playback exceeds retained timing allowance')
        result.append({'case':c,'boundary_duration':d,'return_rest':s,'model':m,'absolute_gate_difference_spc_cycles':gd,'absolute_onset_difference_spc_cycles':od,'absolute_pitch_difference_spc_cycles':pd,'absolute_control_difference_spc_cycles':cd,'absolute_stop_difference_spc_cycles':sd})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            if args.reference.stat().st_size>1024*1024:raise ValueError('mixed JSON reference exceeds bound')
            loaded=json.loads(args.reference.read_text())
            if loaded.get('schema')!=REF_SCHEMA or loaded.get('qualification') is not False or loaded.get('playback') is not False:raise ValueError('invalid mixed observation artifact')
            refs=loaded['reference']
        else:
            if not args.trace or not args.firmware_dir:p.error('provide --reference or --trace and --firmware-dir')
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,d,c,s) for c in CASES for s in RESTS for d in DURATIONS for m in ('sgb','sgb2')]
        candidates={f'{c}-{s}-{d}':native(args.probe.resolve(),bank(127,d,c,s)) for c in CASES for s in RESTS for d in DURATIONS}
        result={'schema':'gbb-score-final-mixed-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'native':candidates,'reference':refs,'comparisons':compare(candidates,refs),'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
