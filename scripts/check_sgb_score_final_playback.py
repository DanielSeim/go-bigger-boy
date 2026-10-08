#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate a final raw event, physical stop writes and a bounded held note."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_final import build as program_build
from build_sgb_score_final_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_final_reference import expected,contract,agree,run
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_pending_playback import validate_metadata,align
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_envelope_reference import validate_trajectories
from check_sgb_score_reselect_reference import validate_writes
from check_sgb_score_gate_reference import PULSES
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_duet_reference import integer
SCHEMA='gbb-spc-score-final-v1'


def identity(report):
    events=report['events']
    if len(events)!=5:raise ValueError('final corpus requires five raw records')
    e=events[-1];c='final-rest' if e['opcode']==0xC9 else 'final-note';a,d=e['articulation'],e['duration']
    if type(a) is not int or a not in (63,127) or type(d) is not int or d not in DURATIONS:raise ValueError('unmeasured final profile')
    return c,a,d


def control_writes(report):
    rows=report['key_writes'];stops=[i for i,w in enumerate(rows) if w['address']==0x5C and w['value']==255 and w['half_cycle']>report['keyons'][1]['half_cycle']]
    if len(stops)!=1:raise ValueError('missing/extra physical final stop')
    return rows[stops[0]:stops[0]+3]


def observation(report):
    gates=[];pending={};index=0
    for half,is_on,e in sorted([(e['half_cycle'],True,e) for e in report['keyons']]+[(e['half_cycle'],False,e) for e in report['keyoffs']]):
        for voice in (2,3):
            if not e['mask']&(1<<voice):continue
            if is_on:
                if voice in pending:raise ValueError('final native invented unreleased retrigger')
                pending[voice]=(index,half)
            else:
                if voice not in pending:raise ValueError('final native released an unbound voice')
                onset,start=pending.pop(voice);gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':(half-start)/2,'cause':e['cause']})
        if is_on:index+=1
    return sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),sorted(pending)


def validate(report):
    multi_validate(report,schema=SCHEMA,max_events=64,max_ticks=2032,min_events=1,initial_pair=False,terminal_event=True)
    if report['status']!=2:
        validate_metadata(report,schema=SCHEMA)
        if report.get('key_writes')!=[] or any(report.get(k)!=0 or type(report.get(k)) is not int for k in ('final_mode','final_observation_half_cycles','final_env_start','final_env_end')):raise ValueError('rejected final bank observed a terminal state')
        return
    if report.get('source_unmodified') is not True or report.get('cache_guards_equal') is not True or not integer(report.get('completion_half_cycle'),1,30000000) or report.get('tempo')!=96 or type(report.get('tempo')) is not int:raise ValueError('invalid final source/clock/completion')
    if report.get('final_mode')!=1 or type(report.get('final_mode')) is not int or report.get('final_observation_half_cycles')!=600000 or type(report.get('final_observation_half_cycles')) is not int:raise ValueError('invalid final observation window')
    if report.get('second_pattern_tick')!=0 or type(report.get('second_pattern_tick')) is not int or report.get('end_tick')!=32 or report.get('pattern_ticks')!=[0] or any(type(t) is not int for t in report['pattern_ticks']) or report.get('pattern_masks')!=[12] or any(type(m) is not int for m in report['pattern_masks']) or report.get('event_patterns')!=[0]*5 or any(type(p) is not int for p in report['event_patterns']):raise ValueError('final raw pattern/tick metadata differs')
    for e in report['events']:
        if any(type(e.get(k)) is not int for k in ('articulation','pan','track_volume','song_volume','instrument_sets')) or not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not integer(v,0,127) for v in e['volumes']):raise ValueError('invalid final raw controls')
    c,a,d=identity(report);timed=align(report,bank(a,d,c))
    if not timed[-1].get('boundary_event') or report['events'][-1]['tick']!=report['end_tick']:raise ValueError('final event is not the terminal boundary')
    validate_writes(report)
    writes=report.get('voice_writes');previous=0
    if not isinstance(writes,list) or len(writes)>256:raise ValueError('invalid final DSP write count')
    for w in writes:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,report['completion_half_cycle']) or w.get('address') not in (0x20,0x21,0x22,0x23,0x30,0x31,0x32,0x33) or type(w.get('address')) is not int or not integer(w.get('value'),0,255):raise ValueError('invalid final voice write')
        previous=w['half_cycle']
    count=0
    for i,(e,symbolic) in enumerate(zip(report['events'],timed)):
        stop=report['events'][i+1]['half_cycle'] if i+1<len(timed) else report['completion_half_cycle'];actual=[w for w in writes if e['half_cycle']<=w['half_cycle']<stop];wanted=[]
        if e['opcode']!=0xC9:
            pitch=PITCH[e['opcode']];base=16*e['channel'];wanted=[] if symbolic.get('boundary_event') else [(base,e['volumes'][0]),(base+1,e['volumes'][1])];wanted.extend([(base+2,pitch&255),(base+3,pitch>>8)])
        if [(w['address'],w['value']) for w in actual]!=wanted:raise ValueError('final actual VOL/pitch writes differ')
        count+=len(actual)
    if count!=len(writes):raise ValueError('final write outside raw event')
    for name in ('keyons','keyoffs'):
        values=report.get(name);previous=0
        if not isinstance(values,list) or len(values)>64:raise ValueError('invalid final edge count')
        for e in values:
            if not isinstance(e,dict) or not integer(e.get('half_cycle'),previous+1,report['completion_half_cycle']) or not integer(e.get('tick'),0,32) or any(type(e.get(k)) is not int for k in ('mask','affected_mask','held_mask','cause')) or e['mask'] not in (4,8,12) or e['affected_mask'] not in (4,8,12) or e['held_mask'] not in (0,4,8,12) or e['cause']!=(0 if name=='keyons' else 1):raise ValueError('invalid final physical edge')
            previous=e['half_cycle']
            for key,limit in (('pitches',0x3FFF),('pending_pulses',58)):
                if not isinstance(e.get(key),list) or len(e[key])!=2 or any(not integer(v,0,limit) for v in e[key]):raise ValueError('invalid final edge profile')
            if not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not isinstance(v,list) or len(v)!=2 or any(not integer(x,0,127) for x in v) for v in e['volumes']):raise ValueError('invalid final edge volumes')
    if len(report['keyons'])!=len(expected(c)):raise ValueError('final KON count differs')
    for i,(e,target) in enumerate(zip(report['keyons'],expected(c))):
        group=[x for x in report['events'] if x['tick']==16*i]
        if e['tick']!=16*i or e['mask']!=target['mask'] or e['affected_mask']!=target['mask'] or e['held_mask']!=0 or not 0<e['half_cycle']-group[-1]['half_cycle']<=4096:raise ValueError('final KON grouping differs')
        for v in target['voices']:
            if e['pitches'][v['voice']-2]!=v['pitch'] or e['volumes'][v['voice']-2]!=[7,7] or e['pending_pulses'][v['voice']-2]!=(0 if i==2 else PULSES[(96,a,16)]):raise ValueError('final live note/profile differs')
    keys=report.get('key_writes');previous=0
    if not isinstance(keys,list) or len(keys)>1024:raise ValueError('invalid final key-write count')
    for w in keys:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,report['completion_half_cycle']) or type(w.get('address')) is not int or w['address'] not in (0x4C,0x5C) or type(w.get('value')) is not int or w['value'] not in (0,4,8,12,255) or (w['address']==0x4C and w['value']==255):raise ValueError('invalid final key-register write')
        previous=w['half_cycle']
    # Bind every physical key edge to its actual accepted write, including
    # the stop's FF/00 KOF pulse even when earlier notes already released.
    kon=0;kof=12;on=0;off=0
    for w in keys:
        if w['address']==0x4C:
            if w['value'] and not kon:
                if on>=len(report['keyons']) or (w['half_cycle'],w['value'])!=(report['keyons'][on]['half_cycle'],report['keyons'][on]['mask']):raise ValueError('final KON not bound to register write')
                on+=1
            kon=w['value']
        else:
            asserted=(w['value']&~kof)&12;kof=w['value']
            if asserted:
                if off>=len(report['keyoffs']) or (w['half_cycle'],asserted,kof)!=(report['keyoffs'][off]['half_cycle'],report['keyoffs'][off]['mask'],report['keyoffs'][off]['held_mask']):raise ValueError('final KOF not bound to register write')
                off+=1
    if (on,off)!=(len(report['keyons']),len(report['keyoffs'])) or kon or kof:raise ValueError('final key-register tail differs')
    controls=control_writes(report)
    if [(w['address'],w['value']) for w in controls]!=[(0x5C,255),(0x5C,0),(0x4C,4 if c=='final-note' else 0)] or any(not 0<=w['half_cycle']-controls[0]['half_cycle']<=8192 for w in controls):raise ValueError('final stop sequence differs')
    validate_trajectories(report,max_events=64,clipped_peer=True,held_final=(2,) if c=='final-note' else ())
    gates,held=observation(report)
    if [(g['onset_index'],g['voice']) for g in gates]!=[(0,2),(0,3),(1,2),(1,3)] or held!=([2] if c=='final-note' else []):raise ValueError('final release/held identities differ')
    for g in gates:
        pulses=PULSES[(96,a,16)]
        if g['cause']!=1 or not 2048*(pulses-1)<=g['gate_spc_cycles']<=2048*pulses+4096:raise ValueError('final preceding release profile differs')
    for name in ('peer_checks','frozen_peer_checks','settled_envelope_checks','settled_gate_frames','settled_peer_nonzero_frames'):
        if not isinstance(report.get(name),list) or len(report[name])!=2 or any(not integer(v,0,30000000) for v in report[name]):raise ValueError('invalid final lifecycle counters')
    pcm=report.get('pcm');tail=report.get('second_tail_pcm')
    if not isinstance(pcm,dict) or not isinstance(tail,dict) or not integer(pcm.get('frames'),1,500000) or not integer(pcm.get('peak'),1,32768) or not integer(pcm.get('fnv1a64'),0,2**64-1) or not integer(pcm.get('quiet_tail_frames'),0 if c=='final-note' else 64,pcm['frames']) or not integer(tail.get('frames'),0,0):raise ValueError('invalid final PCM bounds')
    for k in ('left_nonzero_frames','right_nonzero_frames','nonzero_frames'):
        if not integer(pcm.get(k),1,pcm['frames']):raise ValueError('invalid final PCM activity')
    final_pcm=report.get('final_tail_pcm')
    if not isinstance(final_pcm,dict) or not integer(final_pcm.get('frames'),1,pcm['frames']) or not integer(final_pcm.get('fnv1a64'),0,2**64-1) or not integer(final_pcm.get('nonzero_frames'),1 if c=='final-note' else 0,final_pcm['frames'] if c=='final-note' else 0) or not integer(final_pcm.get('peak'),1 if c=='final-note' else 0,32768 if c=='final-note' else 0) or not integer(final_pcm.get('quiet_tail_frames'),0 if c=='final-note' else 64,63 if c=='final-note' else final_pcm['frames']):raise ValueError('final tail audio differs from held/rest state')
    for k in ('final_env_start','final_env_end'):
        if not integer(report.get(k),1 if c=='final-note' else 0,127 if c=='final-note' else 0):raise ValueError('final held envelope state differs')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def pitch_offsets(r):
    stop=control_writes(r)[0]['half_cycle'];start=r['keyons'][1]['half_cycle']
    return [(stop-w['half_cycle'])/2 for w in r['voice_writes'] if w['address'] in (0x23,0x33) and start<w['half_cycle']<stop]


def compare(candidates,refs):
    agree(refs)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{d}-{a}' for c in CASES for d in DURATIONS for a in (63,127)}:raise ValueError('require complete final native matrix')
    for r in candidates.values():validate(r)
    result=[]
    for ref in refs:
        c,a,d,m=(ref[k] for k in ('case','articulation','boundary_duration','model'));r=candidates[f'{c}-{d}-{a}']
        if ref.get('fixture_sha256')!=hashlib.sha256(build(a,d,c)).hexdigest():raise ValueError('final owned fixture hash differs')
        gates,held=observation(r);controls=control_writes(r);stop=controls[0]['half_cycle']
        gd=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,ref['note_gates'])]
        intervals=[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])];od=[abs(x-y) for x,y in zip(intervals,ref['onset_intervals_spc_cycles'])]
        sd=abs((stop-r['keyons'][1]['half_cycle'])/2-ref['final_stop_interval_spc_cycles'])
        pd=[abs(x-y) for x,y in zip(pitch_offsets(r),ref['boundary_pitch_to_stop_spc_cycles'])]
        cd=[abs((w['half_cycle']-stop)/2-o) for w,o in zip(controls,ref['final_control_offsets_spc_cycles'])]
        if len(pd)!=len(ref['boundary_pitch_writes']) or held!=ref['unreleased_final_voices'] or any(v>4096 for v in gd+od+pd+cd+[sd]):raise ValueError('final exceeds retained allowance')
        result.append({'case':c,'articulation':a,'boundary_duration':d,'model':m,'absolute_gate_difference_spc_cycles':gd,'absolute_onset_difference_spc_cycles':od,'absolute_pitch_difference_spc_cycles':pd,'absolute_stop_difference_spc_cycles':sd,'absolute_control_difference_spc_cycles':cd})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:refs=json.loads(args.reference.read_text())['reference']
        elif args.trace and args.firmware_dir:refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for d in DURATIONS for a in (63,127) for m in ('sgb','sgb2')]
        else:raise ValueError('provide saved reference or trace and firmware directory')
        candidates={f'{c}-{d}-{a}':native(args.probe.resolve(),bank(a,d,c)) for c in CASES for d in DURATIONS for a in (63,127)}
        result={'schema':'gbb-score-final-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'reference':refs,'native':candidates,'comparisons':compare(candidates,refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
