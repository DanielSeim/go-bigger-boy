#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque observations of a held voice returning directly with a note."""
import argparse,csv,hashlib,io,json,subprocess
from pathlib import Path
from build_sgb_score_direct_return_fixture import build,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_final_return_reference import expected as prior_expected
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_gate_reference import PULSES
SCHEMA='gbb-score-direct-return-observation-v1'


def expected(case):
    if case not in CASES:raise ValueError('invalid direct-return case')
    result=prior_expected('return-note')
    if case=='direct-note':
        result.append({'mask':4,'voices':[{**result[-1]['voices'][0],'pitch':1800}]})
    return result


def contract(r,case):
    if not isinstance(r,dict):raise ValueError('invalid direct-return observation')
    a,d=(r.get(k) for k in ('return_articulation','return_duration'))
    if case not in CASES or type(a) is not int or a not in ARTICULATIONS or type(d) is not int or d not in DURATIONS or not integer(r.get('initial_articulation'),127,127):raise ValueError('invalid direct-return profile')
    if r.get('keyons')!=expected(case):raise ValueError('direct-return owned setup differs')
    for e in r['keyons']:
        if type(e['mask']) is not int or any(type(v) is not int for voice in e['voices'] for k,v in voice.items() if k!='volumes') or any(type(v) is not int for voice in e['voices'] for v in voice['volumes']):raise ValueError('invalid direct-return setup types')
    intervals=r.get('onset_intervals_spc_cycles');bounds=[(84000,92000)]*4+([(40*2048*d//16,46*2048*d//16+4096)] if case=='direct-note' else [])
    if not isinstance(intervals,list) or len(intervals)!=len(bounds) or any(not integer(v,*b) for v,b in zip(intervals,bounds)):raise ValueError('direct-return onset spacing differs')
    gates=r.get('note_gates');identities=[(0,2),(0,3),(1,3),(2,3),(3,3),(4,2)]
    if not isinstance(gates,list) or len(gates)!=6:raise ValueError('direct-return release count differs')
    for g,identity in zip(gates,identities):
        if not isinstance(g,dict) or any(type(g.get(k)) is not int for k in ('voice','onset_index')) or (g['onset_index'],g['voice'])!=identity or g.get('release_kind')!='timer-keyoff':raise ValueError('direct-return release identity/source differs')
        pulses=PULSES[(96,a,d)] if identity==(4,2) else PULSES[(96,127,16)]
        if not integer(g.get('gate_spc_cycles'),2048*(pulses-2),2048*pulses+4096):raise ValueError('direct-return timer gate differs')
    retriggers=r.get('unreleased_retriggers')
    wanted={'voice':2,'onset_index':4,'previous_onset_index':1,'interval_spc_cycles':sum(intervals[1:4])}
    if retriggers!=[wanted] or any(type(v) is not int for v in retriggers[0].values()):raise ValueError('direct return did not retrigger the held voice')
    if r.get('voices_pending_at_stop')!=[] or r.get('unreleased_final_voices')!=([2] if case=='direct-note' else []):raise ValueError('direct-return final lifecycle differs')
    controls=[{'address':92,'value':0},{'address':92,'value':0},{'address':76,'value':4}]
    if r.get('return_control_writes')!=controls or any(type(v) is not int for w in r['return_control_writes'] for v in w.values()):raise ValueError('direct-return KON pulse differs')
    offsets=r.get('return_control_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=3 or any(not integer(v,-4096,0) for v in offsets) or not offsets[0]<offsets[1]<offsets[2]==0:raise ValueError('direct-return control timing differs')
    writes=[{'address':34,'value':164},{'address':35,'value':6},{'address':32,'value':7},{'address':33,'value':7}]
    if r.get('return_voice_writes')!=writes or any(type(v) is not int for w in r['return_voice_writes'] for v in w.values()):raise ValueError('direct-return actual VOL/pitch differs')
    offsets=r.get('return_voice_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=4 or any(not integer(v,-4096,-1) for v in offsets) or any(x>=y for x,y in zip(offsets,offsets[1:])):raise ValueError('direct-return voice timing differs')
    stop=r.get('final_stop_interval_spc_cycles')
    # Returning KON setup follows the score boundary; allow one timer pulse
    # of phase in this raw KON-to-stop interval without changing timestamps.
    if not integer(stop,40*2048*d//16-2048,46*2048*d//16) or gates[-1]['gate_spc_cycles']>=stop:raise ValueError('returning note was not released before final stop')
    controls=[{'address':92,'value':255},{'address':92,'value':0},{'address':76,'value':4 if case=='direct-note' else 0}]
    if r.get('final_control_writes')!=controls or any(type(v) is not int for w in r['final_control_writes'] for v in w.values()):raise ValueError('direct-return final pulse differs')
    offsets=r.get('final_control_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=3 or any(not integer(v,0,4096) for v in offsets) or not offsets[0]==0<offsets[1]<offsets[2]:raise ValueError('direct-return final control timing differs')
    if case=='direct-note' and intervals[-1]!=stop+offsets[2]:raise ValueError('direct-return final onset is not bound to KON')
    pitch=[{'voice':2,'pitch':1800}] if case=='direct-note' else []
    if r.get('boundary_pitch_writes')!=pitch or any(type(v) is not int for w in r['boundary_pitch_writes'] for v in w.values()):raise ValueError('direct-return final pitch differs')
    offsets=r.get('boundary_pitch_to_stop_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=len(pitch) or any(not integer(v,1,4096) for v in offsets):raise ValueError('direct-return final pitch timing differs')
    if r.get('post_return_volume_writes')!=[] or r.get('post_stop_nonzero_control_writes')!=[] or not integer(r.get('post_stop_zero_control_write_count'),0,32764):raise ValueError('direct-return deferred/post-stop writes differ')
    if r.get('final_volumes')!=[[7,7],[1,1]] or any(type(v) is not int for pair in r['final_volumes'] for v in pair):raise ValueError('direct-return final volume differs')
    if not integer(r.get('post_stop_observation_spc_cycles'),200000,6000000):raise ValueError('direct-return observation window differs')


def observe(source,case,art=63,duration=8):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('direct-return trace exceeds byte bound')
    r=onsets(io.StringIO(text),case,onset_validator=lambda r,c:None)
    rows=[];cycles=[];pending={};gates=[];retriggers=[];regs={};stop=None;held=None;last=0
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        a,v,c=(int(row[k]) for k in ('address','value','spc_cycle'));rows.append((a,v,c));regs[a]=v;last=c
        if a==92:
            if v==255 and len(cycles)>=5:
                if stop is not None:raise ValueError('duplicate direct-return final stop')
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
    if stop is None or len(cycles)<5:raise ValueError('missing direct-return stop/onsets')
    controls=[(a,v,c) for a,v,c in rows if c>=stop and a in (76,92)]
    returning=[(a,v,c) for a,v,c in rows if c<=cycles[4] and a in (76,92)][-3:]
    voice=[(a,v,c) for a,v,c in rows if cycles[3]<c<=cycles[4] and a in (32,33,34,35,48,49,50,51)]
    low={};pitch=[];pc=[];vol=[]
    for a,v,c in rows:
        if a in (34,50):low[a//16]=v
        if cycles[4]<c<stop and a in (35,51):
            if a//16 not in low:raise ValueError('incomplete direct-return final pitch')
            pitch.append({'voice':a//16,'pitch':low[a//16]|v<<8});pc.append(stop-c)
        if c>cycles[4] and a in (32,33,48,49):vol.append({'address':a,'value':v})
    r.update(initial_articulation=127,return_articulation=art,return_duration=duration,note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=retriggers,
             voices_pending_at_stop=held,unreleased_final_voices=sorted(pending),return_control_writes=[{'address':a,'value':v} for a,v,c in returning],return_control_offsets_spc_cycles=[c-cycles[4] for a,v,c in returning],
             return_voice_writes=[{'address':a,'value':v} for a,v,c in voice],return_voice_offsets_spc_cycles=[c-cycles[4] for a,v,c in voice],
             final_stop_interval_spc_cycles=stop-cycles[4],final_control_writes=[{'address':a,'value':v} for a,v,c in controls[:3]],final_control_offsets_spc_cycles=[c-stop for a,v,c in controls[:3]],
             boundary_pitch_writes=pitch,boundary_pitch_to_stop_spc_cycles=pc,post_return_volume_writes=vol,post_stop_nonzero_control_writes=[{'address':a,'value':v} for a,v,c in controls[3:] if v],post_stop_zero_control_write_count=sum(v==0 for a,v,c in controls[3:]),
             final_volumes=[[regs[32],regs[33]],[regs[48],regs[49]]],post_stop_observation_spc_cycles=last-stop)
    contract(r,case);return r


def run(trace,firmware_dir,model,art,duration,case):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,duration,case),observer=lambda src:observe(src,case,art,duration),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000
    return r


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=16:raise ValueError('require complete direct-return matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid direct-return reference')
        c,a,d,m=(r.get(k) for k in ('case','return_articulation','return_duration','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,d,m) in seen:raise ValueError('duplicate/unknown direct-return reference')
        if r.get('fixture_sha256')!=hashlib.sha256(build(a,d,c)).hexdigest():raise ValueError('direct-return owned cartridge hash differs')
        seen.add((c,a,d,m))
    for c in CASES:
        for a in ARTICULATIONS:
            for d in DURATIONS:
                x,y=[r for r in refs if (r['case'],r['return_articulation'],r['return_duration'])==(c,a,d)]
                diffs={}
                for field in ('note_gates','onset_intervals_spc_cycles','return_control_offsets_spc_cycles','return_voice_offsets_spc_cycles','final_control_offsets_spc_cycles','boundary_pitch_to_stop_spc_cycles','unreleased_retriggers'):
                    p,q=x[field],y[field]
                    if field in ('note_gates','unreleased_retriggers'):
                        key='gate_spc_cycles' if field=='note_gates' else 'interval_spc_cycles';p,q=([v[key] for v in z] for z in (p,q))
                    diffs[field]=[abs(v-w) for v,w in zip(p,q)]
                diffs['final_stop_interval_spc_cycles']=[abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])]
                if any(v>2048 for values in diffs.values() for v in values):raise ValueError('direct-return originals exceed retained allowance')
                result.append({'case':c,'return_articulation':a,'return_duration':d,'absolute_differences_spc_cycles':diffs})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for a in ARTICULATIONS for d in DURATIONS for m in ('sgb','sgb2')]
        result={'schema':SCHEMA,'qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
