#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque short-return observations outside immediate final readiness."""
import argparse,csv,hashlib,io,json,subprocess
from pathlib import Path
from build_sgb_score_short_continue_fixture import build,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_direct_return_reference import expected as prior_expected
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_gate_reference import PULSES
SCHEMA='gbb-score-short-continue-observation-v1'


def expected(case):
    if case not in CASES:raise ValueError('invalid short continuation case')
    result=prior_expected('direct-note' if case=='continue-note' else 'direct-rest')
    if case=='continue-note':result[-1]['voices'][0]['volumes']=[1,1]
    return result


def observe(source,case,art=63,duration=8,*,short_follow=False):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('short continuation trace exceeds byte bound')
    result=onsets(io.StringIO(text),case,onset_validator=lambda r,c:None)
    rows=[];cycles=[];pending={};gates=[];retriggers=[];regs={};stop=None;held=None;last=0
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        a,v,c=(int(row[k]) for k in ('address','value','spc_cycle'));rows.append((a,v,c));regs[a]=v;last=c
        if a==92:
            if v==255 and len(cycles)>=5:
                if stop is not None:raise ValueError('duplicate short continuation stop')
                stop=c;held=sorted(pending)
            for voice in (2,3):
                if v&(1<<voice) and voice in pending:
                    old,start=pending.pop(voice);gates.append({'voice':voice,'onset_index':old,'gate_spc_cycles':c-start,'release_kind':'final-stop' if c==stop else 'timer-keyoff'})
        if a==76 and v:
            for voice in (2,3):
                if v&(1<<voice):
                    if voice in pending:
                        old,start=pending[voice];retriggers.append({'voice':voice,'onset_index':len(cycles),'previous_onset_index':old,'interval_spc_cycles':c-start})
                    pending[voice]=(len(cycles),c)
            cycles.append(c)
    if stop is None or len(cycles)<5:raise ValueError('missing short continuation stop/onsets')
    ret=cycles[4];following=cycles[5] if len(cycles)==6 else stop
    controls=[(a,v,c) for a,v,c in rows if c>=stop and a in (76,92)]
    returning=[(a,v,c) for a,v,c in rows if c<=ret and a in (76,92)][-3:]
    continuation=[(a,v,c) for a,v,c in rows if c<=following and a in (76,92)][-3:] if len(cycles)==6 else []
    voice=[(a,v,c) for a,v,c in rows if cycles[3]<c<=ret and a in (32,33,34,35,48,49,50,51)]
    after=[(a,v,c) for a,v,c in rows if ret<c<stop and a in (32,33,34,35,48,49,50,51)]
    def writes(ws):return [{'address':a,'value':v} for a,v,c in ws]
    result.update(return_articulation=art,return_duration=4,continuation_duration=duration,
        note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=retriggers,
        voices_pending_at_stop=held,unreleased_final_voices=sorted(pending),
        return_control_writes=writes(returning),return_control_offsets_spc_cycles=[c-ret for a,v,c in returning],
        return_voice_writes=writes(voice),return_voice_offsets_spc_cycles=[c-ret for a,v,c in voice],
        continuation_control_writes=writes(continuation),continuation_control_offsets_spc_cycles=[c-following for a,v,c in continuation],
        continuation_voice_writes=writes(after),continuation_voice_offsets_spc_cycles=[c-following for a,v,c in after],
        final_stop_interval_spc_cycles=stop-ret,final_control_writes=writes(controls[:3]),final_control_offsets_spc_cycles=[c-stop for a,v,c in controls[:3]],
        final_volumes=[[regs[32],regs[33]],[regs[48],regs[49]]],post_stop_nonzero_control_writes=writes([w for w in controls[3:] if w[1]]),post_stop_zero_control_write_count=sum(v==0 for a,v,c in controls[3:]),post_stop_observation_spc_cycles=last-stop)
    contract(result,case,short_follow=short_follow)
    return result


def contract(r,case,*,short_follow=False):
    if type(short_follow) is not bool:raise ValueError('invalid short continuation selector')
    if not isinstance(r,dict) or case not in CASES:raise ValueError('invalid short continuation observation')
    a,d=(r.get(k) for k in ('return_articulation','continuation_duration'))
    if type(a) is not int or a not in ARTICULATIONS or type(d) is not int or d not in ((4,) if short_follow else DURATIONS) or not integer(r.get('return_duration'),4,4):raise ValueError('invalid short continuation profile')
    if r.get('keyons')!=expected(case):raise ValueError('short continuation onset setup differs')
    def exact(field,wanted):
        if r.get(field)!=wanted:raise ValueError('short continuation '+field+' differs')
        def integers(value):
            if isinstance(value,(list,dict)):
                for item in (value.values() if isinstance(value,dict) else value):integers(item)
            elif not isinstance(value,str) and type(value) is not int:raise ValueError('invalid short continuation integer')
        integers(r[field])
    exact('keyons',expected(case))
    intervals=r.get('onset_intervals_spc_cycles')
    bounds=[(84000,92000)]*4+([(8*2048,12*2048)] if case=='continue-note' else [])
    if not isinstance(intervals,list) or len(intervals)!=len(bounds) or any(not integer(v,*b) for v,b in zip(intervals,bounds)):raise ValueError('short continuation onset spacing differs')
    identities=[(0,2),(0,3),(1,3),(2,3),(3,3),(4,2)]+([(5,2)] if case=='continue-note' else [])
    gates=r.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(identities):raise ValueError('short continuation release count differs')
    for g,identity in zip(gates,identities):
        if not isinstance(g,dict) or any(type(g.get(k)) is not int for k in ('voice','onset_index')) or (g['onset_index'],g['voice'])!=identity or g.get('release_kind')!='timer-keyoff':raise ValueError('short continuation release identity differs')
        if short_follow and identity==(4,2):low,high=6144,8192
        elif short_follow and identity==(5,2):low,high=8192,10240
        elif identity==(4,2):low,high=6144,10240
        else:
            pulses=PULSES[(96,a,d)] if identity==(5,2) else PULSES[(96,127,16)]
            low,high=2048*(pulses-2),2048*pulses+4096
        if not integer(g.get('gate_spc_cycles'),low,high):raise ValueError('short continuation gate differs')
    exact('unreleased_retriggers',[{'voice':2,'onset_index':4,'previous_onset_index':1,'interval_spc_cycles':sum(intervals[1:4])}])
    exact('voices_pending_at_stop',[]);exact('unreleased_final_voices',[])
    controls=[{'address':92,'value':0},{'address':92,'value':0},{'address':76,'value':4}]
    exact('return_control_writes',controls)
    exact('return_voice_writes',[{'address':34,'value':164},{'address':35,'value':6},{'address':32,'value':7},{'address':33,'value':7}])
    exact('continuation_control_writes',controls if case=='continue-note' else [])
    exact('continuation_voice_writes',[{'address':34,'value':8},{'address':35,'value':7},{'address':32,'value':1},{'address':33,'value':1}] if case=='continue-note' else [])
    for prefix in ('return','continuation'):
        for kind in ('control','voice'):
            offsets=r.get(prefix+'_'+kind+'_offsets_spc_cycles');count=len(r[prefix+'_'+kind+'_writes'])
            if not isinstance(offsets,list) or len(offsets)!=count or any(not integer(v,-4096,0 if kind=='control' else -1) for v in offsets) or any(x>=y for x,y in zip(offsets,offsets[1:])) or kind=='control' and offsets and offsets[-1]!=0:raise ValueError('short continuation setup/control timing differs')
    stop=r.get('final_stop_interval_spc_cycles')
    if not integer(stop,(4+d)*5500-8192,(4+d)*5500+8192):raise ValueError('short continuation final spacing differs')
    if gates[5]['gate_spc_cycles']>= (intervals[4] if case=='continue-note' else stop):raise ValueError('short returning note not released before progression')
    if case=='continue-note' and intervals[4]+gates[6]['gate_spc_cycles']>=stop:raise ValueError('continuing note not released before stop')
    exact('final_control_writes',[{'address':92,'value':255},{'address':92,'value':0},{'address':76,'value':0}])
    offsets=r.get('final_control_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=3 or any(not integer(v,0,4096) for v in offsets) or not offsets[0]==0<offsets[1]<offsets[2]:raise ValueError('short continuation final controls differ')
    exact('final_volumes',[[1,1],[1,1]] if case=='continue-note' else [[7,7],[1,1]])
    exact('post_stop_nonzero_control_writes',[])
    if not integer(r.get('post_stop_zero_control_write_count'),0,32764) or not integer(r.get('post_stop_observation_spc_cycles'),200000,6000000):raise ValueError('short continuation observation window differs')


def run(trace,firmware_dir,model,art,duration,case):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,duration,case),observer=lambda src:observe(src,case,art,duration),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000
    return r


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=16:raise ValueError('require complete short continuation matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid short continuation reference')
        c,a,d,m=(r.get(k) for k in ('case','return_articulation','continuation_duration','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,d,m) in seen:raise ValueError('duplicate/unknown short continuation reference')
        if r.get('fixture_sha256')!=hashlib.sha256(build(a,d,c)).hexdigest():raise ValueError('short continuation cartridge hash differs')
        seen.add((c,a,d,m))
    for c in CASES:
        for a in ARTICULATIONS:
            for d in DURATIONS:
                x,y=[r for r in refs if (r['case'],r['return_articulation'],r['continuation_duration'])==(c,a,d)]
                diffs={}
                for field in ('note_gates','onset_intervals_spc_cycles','return_control_offsets_spc_cycles','return_voice_offsets_spc_cycles','continuation_control_offsets_spc_cycles','continuation_voice_offsets_spc_cycles','final_control_offsets_spc_cycles','unreleased_retriggers'):
                    p,q=x[field],y[field]
                    if field in ('note_gates','unreleased_retriggers'):
                        key='gate_spc_cycles' if field=='note_gates' else 'interval_spc_cycles';p,q=([v[key] for v in z] for z in (p,q))
                    if len(p)!=len(q):raise ValueError('short continuation vector lengths differ')
                    diffs[field]=[abs(v-w) for v,w in zip(p,q)]
                diffs['final_stop_interval_spc_cycles']=[abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])]
                if any(v>2048 for values in diffs.values() for v in values):raise ValueError('short continuation originals exceed retained allowance')
                result.append({'case':c,'return_articulation':a,'continuation_duration':d,'absolute_differences_spc_cycles':diffs})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for a in ARTICULATIONS for d in DURATIONS for m in ('sgb','sgb2')]
        result={'schema':SCHEMA,'qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
