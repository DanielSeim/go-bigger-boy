#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque final-stop observations with active/inactive clipped peers."""
import argparse,csv,io,json,subprocess
from pathlib import Path
from build_sgb_score_final_peer_fixture import build,expected,CASES
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_gate_reference import PULSES

SCHEMA='gbb-score-final-peer-observation-v1'


def contract(r,case):
    art=r.get('articulation')
    if case not in CASES or type(art) is not int or art not in (63,127):
        raise ValueError('invalid final peer profile')
    if r.get('keyons')!=expected(case):raise ValueError('final peer setup differs')
    for edge in r['keyons']:
        if type(edge['mask']) is not int or any(type(x) is not int for v in edge['voices'] for k,x in v.items() if k!='volumes') or any(type(x) is not int for v in edge['voices'] for x in v['volumes']):
            raise ValueError('final peer setup types differ')
    intervals=r.get('onset_intervals_spc_cycles')
    if not isinstance(intervals,list) or len(intervals)!=len(expected(case))-1 or any(not integer(x,84000,92000) for x in intervals):
        raise ValueError('final peer onset spacing differs')
    # Stop writes precede next-pattern setup; use whole timer-pulse bounds.
    if not integer(r.get('final_stop_interval_spc_cycles'),40*2048,46*2048):
        raise ValueError('final peer stop tick differs')
    peer=5-int(case[-1]);held=[peer] if art==127 else []
    if r.get('voices_pending_at_stop')!=held or any(type(x) is not int for x in r['voices_pending_at_stop']):
        raise ValueError('final peer was not held through stop')
    if r.get('unreleased_retriggers')!=[] or r.get('unreleased_final_voices')!=[]:
        raise ValueError('final peer retriggered or remained held')
    controls=r.get('final_control_writes')
    if controls!=[{'address':92,'value':255},{'address':92,'value':0},{'address':76,'value':0}] or any(type(x) is not int for w in controls for x in w.values()):
        raise ValueError('final peer stop pulse differs')
    offsets=r.get('final_control_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=3 or any(not integer(x,0,4096) for x in offsets) or offsets[0]!=0 or not offsets[0]<offsets[1]<offsets[2]:
        raise ValueError('final peer stop pulse timing differs')
    if r.get('post_stop_nonzero_control_writes')!=[] or not integer(r.get('post_stop_zero_control_write_count'),0,32764):
        raise ValueError('final peer post-stop control activity differs')
    gates=r.get('note_gates');identities=[(i,v['voice']) for i,e in enumerate(expected(case)) for v in e['voices']]
    if not isinstance(gates,list) or len(gates)!=len(identities):raise ValueError('final peer release count differs')
    for g,identity in zip(gates,identities):
        if not isinstance(g,dict) or any(type(g.get(k)) is not int for k in ('voice','onset_index')) or (g['onset_index'],g['voice'])!=identity:
            raise ValueError('final peer release identity differs')
        stop_release=art==127 and identity==(1,peer)
        if g.get('release_kind')!=('final-stop' if stop_release else 'timer-keyoff'):
            raise ValueError('final peer release source differs')
        if stop_release:
            count=3 if case.startswith('inactive') else 1
            low,high=40*2048*count,46*2048*count
        else:
            pulses=PULSES[(96,art,24 if identity==(1,peer) else 16)]
            low,high=2048*(pulses-2),2048*pulses+4096
        if not integer(g.get('gate_spc_cycles'),low,high):raise ValueError('final peer gate spacing differs')
    wanted=[[7,7],[7,7]]
    if case.startswith('inactive'):wanted[int(case[-1])-2]=[1,1]
    if r.get('final_volumes')!=wanted or any(type(x) is not int for p in r['final_volumes'] for x in p):
        raise ValueError('final peer volumes differ')
    if not integer(r.get('post_stop_observation_spc_cycles'),200000,6000000):
        raise ValueError('final peer observation window differs')


def observe(source,case,art=127):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('final peer trace exceeds byte bound')
    result=onsets(io.StringIO(text),case,onset_validator=lambda r,c:None)
    pending={};gates=[];retriggers=[];index=0;rows=[];cycles=[];regs={};held=None;stop=None;last=0
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        a,v,c=(int(row[k]) for k in ('address','value','spc_cycle'))
        rows.append((a,v,c));regs[a]=v;last=c
        if a==92:
            if v==255 and index>=2:
                if stop is not None:raise ValueError('duplicate final peer stop')
                stop=c;held=sorted(pending)
            for voice in (2,3):
                if v&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice)
                    gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':c-start,
                                  'release_kind':'final-stop' if stop==c else 'timer-keyoff'})
        if a==76 and v:
            for voice in (2,3):
                if v&(1<<voice):
                    if voice in pending:retriggers.append(voice)
                    pending[voice]=(index,c)
            index+=1;cycles.append(c)
    if stop is None or not cycles:raise ValueError('missing final peer stop')
    controls=[(a,v,c) for a,v,c in rows if c>=stop and a in (76,92)]
    result.update(articulation=art,note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),
                  voices_pending_at_stop=held,unreleased_retriggers=retriggers,unreleased_final_voices=sorted(pending),
                  final_stop_interval_spc_cycles=stop-cycles[-1],
                  final_control_writes=[{'address':a,'value':v} for a,v,c in controls[:3]],
                  final_control_offsets_spc_cycles=[c-stop for a,v,c in controls[:3]],
                  post_stop_nonzero_control_writes=[{'address':a,'value':v} for a,v,c in controls[3:] if v],
                  post_stop_zero_control_write_count=sum(v==0 for a,v,c in controls[3:]),
                  final_volumes=[[regs[32],regs[33]],[regs[48],regs[49]]],
                  post_stop_observation_spc_cycles=last-stop)
    contract(result,case);return result


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,case),
                      observer=lambda source:observe(source,case,art),contract=contract,pattern_durations=None)
    result['instruction_limit']=8000000;return result


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=16:raise ValueError('require complete final peer matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid final peer reference')
        c,a,m=(r.get(k) for k in ('case','articulation','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,m) in seen:raise ValueError('duplicate/unknown final peer reference')
        seen.add((c,a,m))
    for c in CASES:
        for a in (63,127):
            x,y=[r for r in refs if (r['case'],r['articulation'])==(c,a)]
            gates=[abs(p['gate_spc_cycles']-q['gate_spc_cycles']) for p,q in zip(x['note_gates'],y['note_gates'])]
            onsets=[abs(p-q) for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])]
            controls=[abs(p-q) for p,q in zip(x['final_control_offsets_spc_cycles'],y['final_control_offsets_spc_cycles'])]
            stop=abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])
            if any(v>2048 for v in gates+onsets+controls+[stop]):raise ValueError('final peer originals exceed retained allowance')
            result.append({'case':c,'articulation':a,'absolute_gate_difference_spc_cycles':gates,
                           'absolute_onset_difference_spc_cycles':onsets,'absolute_control_difference_spc_cycles':controls,
                           'absolute_stop_difference_spc_cycles':stop})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,c) for c in CASES for a in (63,127) for m in ('sgb','sgb2')]
        result={'schema':SCHEMA,'qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
