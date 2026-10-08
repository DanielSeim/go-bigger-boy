#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate held direct-note return, actual setup, envelopes and final audio."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_direct_return import build as program_build
from build_sgb_score_direct_return_fixture import bank,build,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_direct_return_reference import expected,agree,run,SCHEMA as REF_SCHEMA
from check_sgb_score_final_return_playback import validate as prior_validate,SCHEMA as PRIOR_SCHEMA
from check_sgb_score_final_playback import observation,control_writes
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_duet_reference import integer
SCHEMA='gbb-spc-score-direct-return-v1'


def identity(r,*,short_return=False):
    es=r['events']
    if len(es)!=9:raise ValueError('direct return requires nine records')
    a,d=es[6]['articulation'],es[-1]['duration'] if short_return else es[6]['duration'];c='direct-rest' if es[-1]['opcode']==0xC9 else 'direct-note'
    if type(a) is not int or a not in ARTICULATIONS or type(d) is not int or d not in DURATIONS or es[6]['opcode']!=0xA0:raise ValueError('unmeasured direct return')
    if short_return and es[6]['duration']!=4:raise ValueError('invalid short returning duration')
    return c,a,d


def validate(r,*,short_return=False,fixture_bank=bank):
    if r.get('schema')!=SCHEMA:raise ValueError('invalid direct-return schema')
    if not integer(r.get('direct_return_mode'),1 if r.get('status')==2 else 0,1 if r.get('status')==2 else 0):raise ValueError('invalid direct-return mode')
    if r.get('status')!=2:
        prior_validate({**r,'schema':PRIOR_SCHEMA});return
    c,a,d=identity(r,short_return=short_return)
    prior_validate({**r,'schema':PRIOR_SCHEMA},direct_return=True,fixture_bank=lambda _a,_d,_c,_s:fixture_bank(a,d,c),expected_onsets=lambda _:expected(c),short_return=short_return)
    retriggers=[e for e in r['envelopes'] if e['retrigger_half_cycle']]
    if len(retriggers)!=1 or retriggers[0]['voice']!=2 or retriggers[0]['on_half_cycle']!=r['keyons'][1]['half_cycle'] or retriggers[0]['retrigger_half_cycle']!=r['keyons'][4]['half_cycle']:raise ValueError('direct-return envelope retrigger differs')
    if r['keyons'][4]['held_mask']!=0:raise ValueError('direct-return KOF retained a held peer bit')
    keys=r['key_writes'];kon=r['keyons'][4]['half_cycle']
    pulse=[w for w in keys if w['half_cycle']<=kon][-3:]
    if [(w['address'],w['value']) for w in pulse]!=[(92,0),(92,0),(76,4)]:raise ValueError('direct-return raw KOF/KON pulse differs')
    if any(not -8192<=w['half_cycle']-kon<=0 for w in pulse):raise ValueError('direct-return raw control interval differs')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,refs,*,short_return=False,validator=validate,reference_agreement=agree):
    reference_agreement(refs)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{a}-{d}' for c in CASES for a in ARTICULATIONS for d in DURATIONS}:raise ValueError('require complete native direct-return matrix')
    result=[]
    for ref in refs:
        c,a,d,m=(ref[k] for k in ('case','return_articulation','boundary_duration' if short_return else 'return_duration','model'));r=candidates[f'{c}-{a}-{d}'];validator(r)
        if identity(r,short_return=short_return)!=(c,a,d):raise ValueError('direct-return native identity differs')
        gates,held=observation(r,allow_retrigger=True);controls=control_writes(r);stop=controls[0]['half_cycle'];kon=r['keyons'][4]['half_cycle']
        values={
            'note_gates':[(g['gate_spc_cycles']) for g in gates],
            'onset_intervals_spc_cycles':[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])],
            'final_control_offsets_spc_cycles':[(w['half_cycle']-stop)/2 for w in controls],
            'return_control_offsets_spc_cycles':[(w['half_cycle']-kon)/2 for w in r['key_writes'] if w['half_cycle']<=kon][-3:],
            'return_voice_offsets_spc_cycles':[(w['half_cycle']-kon)/2 for w in r['voice_writes'] if r['events'][6]['half_cycle']<=w['half_cycle']<r['events'][7]['half_cycle']],
            'boundary_pitch_to_stop_spc_cycles':[(stop-w['half_cycle'])/2 for w in r['voice_writes'] if w['address']==35 and w['half_cycle']>kon],
            'final_stop_interval_spc_cycles':[(stop-kon)/2],
            'unreleased_retriggers':[(kon-r['keyons'][1]['half_cycle'])/2],
        }
        diffs={}
        for field,vs in values.items():
            wanted=ref[field]
            if field in ('note_gates','unreleased_retriggers'):wanted=[v['gate_spc_cycles' if field=='note_gates' else 'interval_spc_cycles'] for v in wanted]
            elif field=='final_stop_interval_spc_cycles':wanted=[wanted]
            if len(vs)!=len(wanted):raise ValueError('direct-return timing vector differs')
            diffs[field]=[abs(x-y) for x,y in zip(vs,wanted)]
        if held!=ref['unreleased_final_voices'] or any(v>4096 for vs in diffs.values() for v in vs):raise ValueError('direct return exceeds retained timing allowance')
        result.append({'case':c,'return_articulation':a,'return_duration':4 if short_return else d,'boundary_duration':d,'model':m,'absolute_differences_spc_cycles':diffs})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            if args.reference.stat().st_size>1024*1024:raise ValueError('direct-return reference exceeds JSON bound')
            loaded=json.loads(args.reference.read_text())
            if loaded.get('schema')!=REF_SCHEMA or loaded.get('qualification') is not False or loaded.get('playback') is not False:raise ValueError('invalid direct-return reference artifact')
            refs=loaded['reference']
        else:
            if not args.trace or not args.firmware_dir:p.error('provide --reference or --trace and --firmware-dir')
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for a in ARTICULATIONS for d in DURATIONS for m in ('sgb','sgb2')]
        candidates={f'{c}-{a}-{d}':native(args.probe.resolve(),bank(a,d,c)) for c in CASES for a in ARTICULATIONS for d in DURATIONS}
        result={'schema':'gbb-score-direct-return-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'native':candidates,'reference':refs,'comparisons':compare(candidates,refs),'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
