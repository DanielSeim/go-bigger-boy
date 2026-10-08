#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque final note/rest observations after clipped-voice inactivity."""
import argparse,csv,io,json,subprocess
from pathlib import Path
from build_sgb_score_final_return_fixture import build,CASES,RESTS,DURATIONS
from build_sgb_score_final_peer_fixture import expected as peer_expected
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_gate_reference import PULSES
SCHEMA='gbb-score-final-return-observation-v1'


def expected(case):
    if case not in CASES:raise ValueError('invalid final return case')
    result=peer_expected('inactive-3')
    if case=='return-note':result.append({'mask':4,'voices':[{'voice':2,'pitch':1700,'volumes':[7,7],'srcn':2,'adsr1':143,'adsr2':111,'gain':184}]})
    return result


def contract(r,case,*,return_art=None):
    a,d,s=(r.get(k) for k in ('articulation','boundary_duration','return_rest'))
    if type(a) is not int or a not in (63,127) or type(d) is not int or d not in DURATIONS or type(s) is not int or s not in RESTS or case not in CASES:
        raise ValueError('invalid final return profile')
    if return_art is not None and (a!=127 or return_art!=63 or r.get('return_articulation')!=63):raise ValueError('invalid mixed returning articulation')
    if r.get('keyons')!=expected(case):raise ValueError('final return setup differs')
    for e in r['keyons']:
        if type(e['mask']) is not int or any(type(v) is not int for x in e['voices'] for k,v in x.items() if k!='volumes') or any(type(v) is not int for x in e['voices'] for v in x['volumes']):raise ValueError('invalid final return setup types')
    intervals=r.get('onset_intervals_spc_cycles')
    bounds=[(84000,92000)]*3+([(84000*(16+s)//16,92000*(16+s)//16)] if case=='return-note' else [])
    if not isinstance(intervals,list) or len(intervals)!=len(bounds) or any(not integer(v,*b) for v,b in zip(intervals,bounds)):raise ValueError('final return onset spacing differs')
    if not integer(r.get('final_stop_interval_spc_cycles'),40*2048*(16+s)//16,46*2048*(16+s)//16):raise ValueError('final return stop spacing differs')
    identities=[(0,2),(0,3),(1,2),(1,3),(2,3),(3,3)];gates=r.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=6:raise ValueError('final return preceding release count differs')
    for g,identity in zip(gates,identities):
        if not isinstance(g,dict) or any(type(g.get(k)) is not int for k in ('voice','onset_index')) or (g['onset_index'],g['voice'])!=identity or g.get('release_kind')!='timer-keyoff':raise ValueError('final return release identity/source differs')
        pulses=PULSES[(96,a,24 if identity==(1,2) else 16)]
        low,high=(240000,340000) if a==127 and identity==(1,2) else (2048*(pulses-2),2048*pulses+4096)
        if not integer(g.get('gate_spc_cycles'),low,high):raise ValueError('final return preceding gate differs')
    after=r.get('return_release_after_previous_onset_spc_cycles')
    if a==127:
        pulses=5 if s==4 else (9 if return_art==63 else PULSES[(96,127,8)])
        if not integer(after,84000+2048*(pulses-2),92000+2048*pulses+4096):raise ValueError('return rest did not govern held release')
        if after!=gates[2]['gate_spc_cycles']-sum(intervals[1:3]):raise ValueError('return release interval is not bound to raw gate/onsets')
    elif after is not None:raise ValueError('already released voice claimed a returning-rest gate')
    if r.get('voices_pending_at_stop')!=[] or r.get('unreleased_retriggers')!=[] or r.get('unreleased_final_voices')!=([2] if case=='return-note' else []):raise ValueError('final return held/retrigger lifecycle differs')
    controls=r.get('final_control_writes')
    if controls!=[{'address':92,'value':255},{'address':92,'value':0},{'address':76,'value':4 if case=='return-note' else 0}] or any(type(v) is not int for w in controls for v in w.values()):raise ValueError('final return KOF/KON pulse differs')
    offsets=r.get('final_control_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=3 or any(not integer(v,0,4096) for v in offsets) or offsets[0]!=0 or not offsets[0]<offsets[1]<offsets[2]:raise ValueError('final return control timing differs')
    if case=='return-note' and intervals[-1]!=r['final_stop_interval_spc_cycles']+offsets[2]:raise ValueError('final return onset is not bound to final KON')
    pitch=[{'voice':2,'pitch':1700}] if case=='return-note' else []
    if r.get('boundary_pitch_writes')!=pitch or any(type(v) is not int for w in r['boundary_pitch_writes'] for v in w.values()):raise ValueError('final return pending pitch differs')
    po=r.get('boundary_pitch_to_stop_spc_cycles')
    if not isinstance(po,list) or len(po)!=len(pitch) or any(not integer(v,1,4096) for v in po):raise ValueError('final return pending pitch timing differs')
    if r.get('post_previous_onset_volume_writes')!=[] or r.get('post_stop_nonzero_control_writes')!=[] or not integer(r.get('post_stop_zero_control_write_count'),0,32764):raise ValueError('final return deferred/post-stop DSP writes differ')
    if r.get('final_volumes')!=[[7,7],[1,1]] or any(type(v) is not int for p in r['final_volumes'] for v in p):raise ValueError('final return actual volume differs')
    if not integer(r.get('post_stop_observation_spc_cycles'),200000,6000000):raise ValueError('final return observation window differs')


def observe(source,case,art=127,duration=8,rest=4,*,return_art=None):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('final return trace exceeds byte bound')
    r=onsets(io.StringIO(text),case,onset_validator=lambda r,c:None)
    rows=[];cycles=[];pending={};gates=[];retriggers=[];regs={};index=0;stop=None;held=None;last=0
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        a,v,c=(int(row[k]) for k in ('address','value','spc_cycle'));rows.append((a,v,c));regs[a]=v;last=c
        if a==92:
            if v==255 and index>=4:
                if stop is not None:raise ValueError('duplicate final return stop')
                stop=c;held=sorted(pending)
            for voice in (2,3):
                if v&(1<<voice) and voice in pending:
                    old,start=pending.pop(voice);gates.append({'voice':voice,'onset_index':old,'gate_spc_cycles':c-start,'release_kind':'final-stop' if c==stop else 'timer-keyoff'})
        if a==76 and v:
            for voice in (2,3):
                if v&(1<<voice):
                    if voice in pending:retriggers.append(voice)
                    pending[voice]=(index,c)
            cycles.append(c);index+=1
    if stop is None or len(cycles)<4:raise ValueError('missing final return stop/onsets')
    controls=[(a,v,c) for a,v,c in rows if c>=stop and a in (76,92)]
    low={};pitch=[];pc=[];vol=[]
    for a,v,c in rows:
        if a in (34,50):low[a//16]=v
        if cycles[3]<c<stop and a in (35,51):
            if a//16 not in low:raise ValueError('incomplete final return pitch')
            pitch.append({'voice':a//16,'pitch':low[a//16]|v<<8});pc.append(stop-c)
        if c>cycles[3] and a in (32,33,48,49):vol.append({'address':a,'value':v})
    gate=next((g for g in gates if (g['onset_index'],g['voice'])==(1,2)),None)
    after=cycles[1]+gate['gate_spc_cycles']-cycles[3] if art==127 and gate else None
    r.update(articulation=art,boundary_duration=duration,return_rest=rest,note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),
             return_release_after_previous_onset_spc_cycles=after,voices_pending_at_stop=held,
             unreleased_retriggers=retriggers,unreleased_final_voices=sorted(pending),final_stop_interval_spc_cycles=stop-cycles[3],
             final_control_writes=[{'address':a,'value':v} for a,v,c in controls[:3]],final_control_offsets_spc_cycles=[c-stop for a,v,c in controls[:3]],
             post_stop_nonzero_control_writes=[{'address':a,'value':v} for a,v,c in controls[3:] if v],post_stop_zero_control_write_count=sum(v==0 for a,v,c in controls[3:]),
             boundary_pitch_writes=pitch,boundary_pitch_to_stop_spc_cycles=pc,post_previous_onset_volume_writes=vol,
             final_volumes=[[regs[32],regs[33]],[regs[48],regs[49]]],post_stop_observation_spc_cycles=last-stop)
    if return_art is not None:r['return_articulation']=return_art
    contract(r,case,return_art=return_art);return r


def run(trace,firmware_dir,model,art,duration,case,rest):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,duration,case,rest),observer=lambda src:observe(src,case,art,duration,rest),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000;return r


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=32:raise ValueError('require complete final return matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid final return reference')
        c,a,d,s,m=(r.get(k) for k in ('case','articulation','boundary_duration','return_rest','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,d,s,m) in seen:raise ValueError('duplicate/unknown final return reference')
        seen.add((c,a,d,s,m))
    for c in CASES:
        for a in (63,127):
            for d in DURATIONS:
                for s in RESTS:
                    x,y=[r for r in refs if (r['case'],r['articulation'],r['boundary_duration'],r['return_rest'])==(c,a,d,s)]
                    gates=[abs(p['gate_spc_cycles']-q['gate_spc_cycles']) for p,q in zip(x['note_gates'],y['note_gates'])]
                    onset=[abs(p-q) for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])]
                    control=[abs(p-q) for p,q in zip(x['final_control_offsets_spc_cycles'],y['final_control_offsets_spc_cycles'])]
                    pitch=[abs(p-q) for p,q in zip(x['boundary_pitch_to_stop_spc_cycles'],y['boundary_pitch_to_stop_spc_cycles'])]
                    stop=abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])
                    if any(v>2048 for v in gates+onset+control+pitch+[stop]):raise ValueError('final return originals exceed retained allowance')
                    result.append({'case':c,'articulation':a,'boundary_duration':d,'return_rest':s,'absolute_gate_difference_spc_cycles':gates,'absolute_onset_difference_spc_cycles':onset,'absolute_control_difference_spc_cycles':control,'absolute_pitch_difference_spc_cycles':pitch,'absolute_stop_difference_spc_cycles':stop})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c,s) for c in CASES for s in RESTS for d in DURATIONS for a in (63,127) for m in ('sgb','sgb2')]
        result={'schema':SCHEMA,'qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
