#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate executed boundary events separately from actual KON and timer gates."""
import argparse,csv,io,json,hashlib,subprocess
from pathlib import Path
from build_sgb_score_reverse_fixture import bank,build,expected,ticks,CASES
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_peer_reference import validate as peer_validate,native_observation,SCHEMA as PEER_SCHEMA
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_mix_reference import selected
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
SCHEMA='gbb-spc-score-reverse-v1'


def onset_contract(result,case):
    if case not in CASES or result.get('keyons')!=expected(case):raise ValueError('reverse owned onset contract differs')
    for edge in result['keyons']:
        if type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices'] for key,value in voice.items() if key!='volumes') or any(type(value) is not int for voice in edge['voices'] for value in voice['volumes']):raise ValueError('reverse setup must contain integers')
    values=result.get('onset_intervals_spc_cycles');points=ticks(case)
    if not isinstance(values,list) or len(values)!=7 or any(not integer(v,84000*(b-a)//16,92000*(b-a)//16) for v,a,b in zip(values,points,points[1:])):raise ValueError('reverse onset interval differs')


def contract(result,case):
    onset_contract(result,case)
    pitches=[{'voice':2,'pitch':1200}]
    if case in ('end-3-note','inherit-note'):pitches.insert(0,{'voice':2,'pitch':1700})
    if result.get('boundary_pitch_writes')!=pitches or any(type(v) is not int for p in result['boundary_pitch_writes'] for v in p.values()):raise ValueError('reverse pitch overwrite differs')
    offsets=result.get('boundary_pitch_to_KON_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=len(pitches) or any(not integer(v,0,4096) for v in offsets):raise ValueError('reverse boundary pitch timing differs')
    wanted=[(i,v['voice']) for i,e in enumerate(expected(case)) for v in e['voices']]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=12 or any(not isinstance(g,dict) or type(g.get('voice')) is not int or type(g.get('onset_index')) is not int or (g['onset_index'],g['voice'])!=target or not integer(g.get('gate_spc_cycles'),1,200000) or g.get('release_kind')!='keyoff' for g,target in zip(gates,wanted)):raise ValueError('reverse releases differ')
    if result.get('unreleased_retriggers')!=[] or result.get('unreleased_final_voices')!=[]:raise ValueError('reverse unexpected held voice')


def observe(source,case):
    # Preserve the preceding observer and contracts; this layer has shorter
    # inherited durations, so it owns a separate interval contract.
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('reverse trace exceeds byte bound')
    result=onsets(io.StringIO(text),case,onset_validator=onset_contract)
    pending={};gates=[];retriggers=[];index=0;low={};writes=[];write_cycles=[];boundary_kon=None
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value,cycle=(int(row[key]) for key in ('address','value','spc_cycle'))
        if address in (0x22,0x32):low[address//16]=value
        if address in (0x23,0x33) and index==2:
            voice=address//16
            if voice not in low:raise ValueError('incomplete reverse pitch')
            writes.append({'voice':voice,'pitch':low[voice]|value<<8});write_cycles.append(cycle)
        if address==0x5C:
            for voice in (2,3):
                if value&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice)
                    gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':cycle-start,'release_kind':'keyoff'})
        if address==0x4C and value:
            if index==2:boundary_kon=cycle
            for voice in (2,3):
                if value&(1<<voice):
                    if voice in pending:retriggers.append(voice)
                    pending[voice]=(index,cycle)
            index+=1
    result.update(boundary_pitch_writes=writes,boundary_pitch_to_KON_spc_cycles=[boundary_kon-cycle for cycle in write_cycles],note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=retriggers,unreleased_final_voices=sorted(pending))
    contract(result,case)
    return result


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:build(art,name),observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    return result


def projection(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA:raise ValueError('invalid reverse schema')
    multi_validate(report,schema=SCHEMA,max_events=64,max_ticks=2032,min_events=1,initial_pair=False)
    events=report['events'];patterns=report.get('event_patterns');ticks_=report.get('pattern_ticks');masks=report.get('pattern_masks')
    if not isinstance(patterns,list) or len(patterns)!=len(events) or not isinstance(ticks_,list) or not isinstance(masks,list) or len(ticks_)!=len(masks):raise ValueError('invalid reverse event patterns')
    if len(ticks_)>4 or any(not integer(t,0,2032) for t in ticks_) or any(type(m) is not int or m not in (4,8,12) for m in masks):raise ValueError('invalid reverse pattern metadata')
    for name in ('keyons','keyoffs'):
        if not isinstance(report.get(name),list) or any(not isinstance(e,dict) or not integer(e.get('half_cycle'),1,30000000) for e in report[name]):raise ValueError('invalid reverse key edge')
    kept=[];overwritten=[];previous=-1
    for i,(event,p) in enumerate(zip(events,patterns)):
        if not integer(p,0,len(ticks_)-1) or not previous<=p<=previous+1:raise ValueError('reverse event pattern order differs')
        previous=p
        start=ticks_[p];end=ticks_[p+1] if p+1<len(ticks_) else report['end_tick']
        if not integer(start,0,2032) or not integer(end,start+1,2032) or not start<=event['tick']<=end or not masks[p]&(1<<event['channel']):raise ValueError('reverse event outside active pattern')
        if event['tick']==end:
            if p+1>=len(ticks_) or masks[p]!=12 or i+1>=len(events):raise ValueError('unqualified reverse boundary')
            following=events[i+1]
            if event['channel']!=2 or patterns[i+1]!=p+1 or following['channel']!=2 or following['tick']!=end or following['opcode']==0xC9 or not masks[p+1]&4:raise ValueError('reverse event not overwritten by following note')
            if not 0<following['half_cycle']-event['half_cycle']<=4096:raise ValueError('reverse overwrite delayed')
            if event.get('instrument_sets')!=0 or type(event.get('instrument_sets')) is not int:raise ValueError('unqualified reverse instrument write')
            if not integer(event.get('articulation'),63,127) or event['articulation'] not in (63,127) or event['opcode'] not in (*PITCH,0xC9):raise ValueError('invalid reverse boundary event')
            if event['opcode']!=0xC9 and (report['tempo'],event['articulation'],event['duration']) not in PULSES:raise ValueError('unmeasured reverse boundary gate')
            if not isinstance(event.get('volumes'),list) or len(event['volumes'])!=2 or any(not integer(v,0,127) for v in event['volumes']):raise ValueError('invalid reverse boundary volumes')
            if any(type(event.get(k)) is not int for k in ('pan','track_volume','song_volume')) or event.get('volumes')!=selected(event['pan'],event['track_volume'],event['song_volume'],event['opcode']==0xC9):raise ValueError('invalid reverse boundary mix')
            if any(event['half_cycle']<=edge['half_cycle']<following['half_cycle'] for edge in report['keyons']+report['keyoffs']):raise ValueError('boundary event fabricated a key edge')
            overwritten.append(i)
        else:kept.append(event)
    projected={**report,'schema':PEER_SCHEMA,'events':kept}
    peer_validate(projected)
    # Actual voice writes must match every executed raw event, including a
    # pitch overwritten before KON. No timestamps or key edges are altered.
    writes=report.get('voice_writes')
    if not isinstance(writes,list) or len(writes)>256:raise ValueError('invalid reverse DSP write count')
    previous_half=0
    for w in writes:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous_half+1,report['completion_half_cycle']) or not integer(w.get('address'),0x20,0x33) or w['address'] not in (0x20,0x21,0x22,0x23,0x30,0x31,0x32,0x33) or not integer(w.get('value'),0,255):raise ValueError('invalid reverse DSP write')
        previous_half=w['half_cycle']
    consumed=0
    for i,e in enumerate(events):
        stop=events[i+1]['half_cycle'] if i+1<len(events) else report['completion_half_cycle']
        actual=[w for w in writes if e['half_cycle']<=w['half_cycle']<stop]
        wanted=[]
        if e['opcode']!=0xC9:
            pitch=PITCH[e['opcode']];base=e['channel']*16
            wanted=[(base,e['volumes'][0]),(base+1,e['volumes'][1]),(base+2,pitch&255),(base+3,pitch>>8)]
        if [(w['address'],w['value']) for w in actual]!=wanted:raise ValueError('executed event DSP writes differ')
        consumed+=len(actual)
    if consumed!=len(writes):raise ValueError('DSP write outside executed event')
    return projected,overwritten


def validate(report):projection(report)


def align(report,data):
    projected,overwritten=projection(report)
    symbolic=schedule(data,int.from_bytes(data[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
    timed=[e for e in symbolic['events'] if e['kind'] in ('note','rest')]
    if report['status']!=2 or report['end_tick']!=symbolic['ticks'] or len(timed)!=len(report['events']) or report['pattern_ticks']!=[p['start_tick'] for p in symbolic['patterns']] or report['pattern_masks']!=[sum(1<<c for c in p['channels']) for p in symbolic['patterns']]:raise ValueError('reverse symbolic timeline differs')
    if overwritten!=[i for i,e in enumerate(timed) if e.get('boundary_event')]:raise ValueError('reverse boundary identities differ')
    controls={2:[10,127],3:[10,127]};counts={2:0,3:0};song=160;index=0
    for event in symbolic['events']:
        c,k=event['channel'],event['kind']
        if k=='pan':controls[c][0]=event['value']
        elif k=='track_volume':controls[c][1]=event['value']
        elif k=='song_volume':song=event['value']
        elif k=='instrument':counts[c]+=1
        elif k in ('note','rest'):
            actual=report['events'][index];p=report['event_patterns'][index];index+=1
            wanted={'tick':event['tick'],'channel':c,'opcode':0xC9 if k=='rest' else 0x80+event['note'],'duration':event['duration'],'articulation':event['articulation']}
            if any(actual[key]!=value for key,value in wanted.items()) or p!=event['pattern']:raise ValueError('raw executed reverse event differs')
            pan,track=controls[c]
            if [actual['pan'],actual['track_volume'],actual['song_volume'],actual['volumes'],actual['instrument_sets']]!=[pan,track,song,selected(pan,track,song,k=='rest'),counts[c]]:raise ValueError('executed reverse inheritance differs')
            counts[c]=0


def native(probe,data,tempo=96):
    from build_sgb_score_reverse import build as program_build
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def boundary_pitches(report):
    # The original observer exports writes between KON groups 1 and 2. Keep
    # the identical physical interval on the native side.
    start,stop=report['keyons'][1]['half_cycle'],report['keyons'][2]['half_cycle'];low={};result=[]
    for w in report['voice_writes']:
        a,v=w['address'],w['value'];voice=a//16
        if a%16==2:low[voice]=v
        if a%16==3 and start<w['half_cycle']<stop:result.append({'voice':voice,'pitch':low[voice]|v<<8})
    return result


def boundary_offsets(report):
    start,stop=report['keyons'][1]['half_cycle'],report['keyons'][2]['half_cycle']
    return [(stop-w['half_cycle'])/2 for w in report['voice_writes'] if w['address']%16==3 and start<w['half_cycle']<stop]


def compare(candidates,references):
    keys={f'{c}-{a}' for c in CASES for a in (63,127)}
    if not isinstance(candidates,dict) or set(candidates)!=keys or not isinstance(references,list) or len(references)!=24:raise ValueError('require complete reverse matrix')
    seen=set();comparisons=[]
    for ref in references:
        if not isinstance(ref,dict):raise ValueError('invalid reverse reference')
        c,a,m=(ref.get(k) for k in ('case','articulation','model'))
        if c not in CASES or type(a) is not int or a not in (63,127) or m not in ('sgb','sgb2') or (c,a,m) in seen:raise ValueError('invalid/duplicate reverse reference')
        seen.add((c,a,m));contract(ref,c)
        r=candidates[f'{c}-{a}'];align(r,bank(a,c))
        if len(r['keyons'])!=8 or boundary_pitches(r)!=ref['boundary_pitch_writes']:raise ValueError('reverse actual pitch overwrite differs')
        for edge,target in zip(r['keyons'],expected(c)):
            if edge['mask']!=target['mask'] or any(edge['pitches'][v['voice']-2]!=v['pitch'] or edge['volumes'][v['voice']-2]!=v['volumes'] for v in target['voices']):raise ValueError('reverse actual KON differs')
        write_diff=[abs(a-b) for a,b in zip(boundary_offsets(r),ref['boundary_pitch_to_KON_spc_cycles'])]
        if len(write_diff)!=len(ref['boundary_pitch_writes']) or any(v>4096 for v in write_diff):raise ValueError('reverse physical pitch timing exceeds retained allowance')
        gates,rs=native_observation(r);original=ref['note_gates']
        if rs or len(gates)!=12 or any((g['onset_index'],g['voice'])!=(o['onset_index'],o['voice']) or g['cause']!=1 for g,o in zip(gates,original)):raise ValueError('reverse actual release identities differ')
        differences=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,original)]
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(r['keyons'],r['keyons'][1:])]
        onset_diff=[abs(a-b) for a,b in zip(intervals,ref['onset_intervals_spc_cycles'])]
        if any(v>4096 for v in differences+onset_diff):raise ValueError('reverse exceeds retained allowance')
        comparisons.append({'case':c,'articulation':a,'model':m,'overwritten_event_indices':projection(r)[1],'absolute_boundary_pitch_difference_spc_cycles':write_diff,'absolute_gate_difference_spc_cycles':differences,'absolute_onset_difference_spc_cycles':onset_diff})
    for c in CASES:
        for a in (63,127):
            x,y=[r for r in references if r['case']==c and r['articulation']==a]
            if x['keyons']!=y['keyons'] or x['boundary_pitch_writes']!=y['boundary_pitch_writes'] or any(abs(p-q)>2048 for p,q in zip(x['boundary_pitch_to_KON_spc_cycles'],y['boundary_pitch_to_KON_spc_cycles'])) or any(abs(p-q)>2048 for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])):raise ValueError('reverse originals disagree')
    return comparisons


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);p.add_argument('--probe',type=Path);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,c) for c in CASES for a in (63,127) for m in ('sgb','sgb2')]
        result={'schema':'gbb-score-reverse-reference-v1','qualification':False,'playback':False,'reference':refs}
        if args.probe:
            from build_sgb_score_reverse import build as program_build
            candidates={f'{c}-{a}':native(args.probe.resolve(),bank(a,c)) for c in CASES for a in (63,127)}
            result.update(native=candidates,comparisons=compare(candidates,refs),program_sha256=hashlib.sha256(program_build()).hexdigest())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
