#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate executed boundary note/rest and immediate rest without projection."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_follow_rest import build as program_build
from build_sgb_score_follow_rest_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_follow_rest_reference import expected,ticks,contract,agree,run
from check_sgb_score_pending_playback import validate_metadata,align
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_envelope_reference import validate_trajectories
from check_sgb_score_reselect_reference import validate_writes
from check_sgb_score_peer_reference import native_observation
from check_sgb_score_reverse_reference import boundary_pitches,boundary_offsets
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_gate_reference import PULSES
from check_sgb_score_duet_reference import integer
SCHEMA='gbb-spc-score-follow-rest-v1'


def identity(report):
    es=report['events'];patterns=report.get('event_patterns')
    if not isinstance(patterns,list) or len(patterns)!=len(es) or any(not integer(p,0,3) for p in patterns):raise ValueError('invalid following-rest raw pattern identities')
    boundary=[e for e,p in zip(es,patterns) if p==0 and e['tick']==32 and e['channel']==2]
    rest=[e for e,p in zip(es,patterns) if p==1 and e['tick']==32 and e['channel']==2]
    if len(boundary)!=1 or len(rest)!=1 or rest[0]['opcode']!=0xC9:raise ValueError('missing following-rest boundary/rest records')
    e,r=boundary[0],rest[0]
    c='boundary-rest' if e['opcode']==0xC9 else 'boundary-note';a,d,s,b=e['articulation'],e['duration'],r['duration'],r['articulation']
    if any(type(v) is not int or v not in (63,127) for v in (a,b)) or any(type(v) is not int or v not in DURATIONS for v in (d,s)):raise ValueError('unmeasured following-rest profile')
    return c,a,d,s,b


def validate(report):
    validate_metadata(report,schema=SCHEMA)
    if report['status']!=2:return
    for e in report['events']:
        if any(type(e.get(k)) is not int for k in ('articulation','pan','track_volume','song_volume','instrument_sets')) or not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not integer(v,0,127) for v in e['volumes']):raise ValueError('invalid following-rest logical controls')
    if type(report.get('second_pattern_tick')) is not int or report['second_pattern_tick']!=32 or not isinstance(report.get('pattern_ticks'),list) or any(not integer(t,0,2032) for t in report['pattern_ticks']) or not isinstance(report.get('pattern_masks'),list) or any(type(m) is not int or m not in (4,8,12) for m in report['pattern_masks']):raise ValueError('invalid following-rest pattern metadata')
    c,a,d,s,b=identity(report);timed=align(report,bank(a,d,s,c,b))
    validate_writes(report);validate_trajectories(report,max_events=64,clipped_peer=True)
    writes=report.get('voice_writes')
    if not isinstance(writes,list) or len(writes)>256:raise ValueError('invalid following-rest DSP write count')
    previous=0
    for w in writes:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,report['completion_half_cycle']) or w.get('address') not in (0x20,0x21,0x22,0x23,0x30,0x31,0x32,0x33) or type(w.get('address')) is not int or not integer(w.get('value'),0,255):raise ValueError('invalid following-rest DSP write')
        previous=w['half_cycle']
    count=0
    for i,(e,symbolic) in enumerate(zip(report['events'],timed)):
        stop=report['events'][i+1]['half_cycle'] if i+1<len(timed) else report['completion_half_cycle']
        actual=[w for w in writes if e['half_cycle']<=w['half_cycle']<stop];wanted=[]
        if e['opcode']!=0xC9:
            pitch=PITCH[e['opcode']];base=16*e['channel']
            wanted=[] if symbolic.get('boundary_event') else [(base,e['volumes'][0]),(base+1,e['volumes'][1])]
            wanted.extend([(base+2,pitch&255),(base+3,pitch>>8)])
        if [(w['address'],w['value']) for w in actual]!=wanted:raise ValueError('following-rest actual VOL/pitch writes differ')
        count+=len(actual)
    if count!=len(writes):raise ValueError('following-rest DSP write outside raw event')
    for name in ('keyons','keyoffs'):
        values=report.get(name);previous=0
        if not isinstance(values,list) or len(values)>64:raise ValueError('invalid following-rest edge count')
        for e in values:
            if not isinstance(e,dict) or not integer(e.get('half_cycle'),previous+1,report['completion_half_cycle']) or not integer(e.get('tick'),0,report['end_tick']) or any(type(e.get(k)) is not int for k in ('mask','affected_mask','held_mask','cause')) or e['mask'] not in (4,8,12) or e['affected_mask'] not in (4,8,12) or e['held_mask'] not in (0,4,8,12) or e['cause']!=(0 if name=='keyons' else 1):raise ValueError('invalid following-rest physical edge')
            previous=e['half_cycle']
            for key,limit in (('pitches',0x3FFF),('pending_pulses',58)):
                if not isinstance(e.get(key),list) or len(e[key])!=2 or any(not integer(v,0,limit) for v in e[key]):raise ValueError('invalid following-rest edge profile')
            if not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not isinstance(v,list) or len(v)!=2 or any(not integer(x,0,127) for x in v) for v in e['volumes']):raise ValueError('invalid following-rest physical volumes')
    if len(report['keyons'])!=8:raise ValueError('following-rest KON count differs')
    for i,(edge,target,tick) in enumerate(zip(report['keyons'],expected(c),ticks(s))):
        group=[e for e in report['events'] if e['tick']==tick]
        affected=sum(1<<v for v in {e['channel'] for e in group})
        if edge['tick']!=tick or edge['mask']!=target['mask'] or edge['affected_mask']!=affected or edge['held_mask']&edge['mask'] or not 0<edge['half_cycle']-group[-1]['half_cycle']<=4096:raise ValueError('following-rest actual KON grouping differs')
        for v in target['voices']:
            if edge['pitches'][v['voice']-2]!=v['pitch'] or edge['volumes'][v['voice']-2]!=v['volumes']:raise ValueError('following-rest live pitch/volume differs')
            pulses=PULSES[(96,b if i in (2,3) else a,s if i==2 else 16)]
            if edge['pending_pulses'][v['voice']-2]!=pulses:raise ValueError('following rest failed to replace pending gate profile')
    held=12
    for half,is_on,e in sorted([(e['half_cycle'],True,e) for e in report['keyons']]+[(e['half_cycle'],False,e) for e in report['keyoffs']]):
        held=(held&~e['mask']) if is_on else (held|e['mask'])
        if e['held_mask']!=held or (not is_on and e['affected_mask']!=e['mask']):raise ValueError('following-rest held KOF differs')
    gates,rs=native_observation(report)
    wanted=[(i,v['voice']) for i,e in enumerate(expected(c)) for v in e['voices']]
    if rs or len(gates)!=len(wanted):raise ValueError('following-rest missing/extra physical release')
    for g,target in zip(gates,wanted):
        profile=PULSES[(96,b if target[0] in (2,3) else a,s if target[0]==2 else 16)]
        if (g['onset_index'],g['voice'])!=target or g['cause']!=1 or not 2048*(profile-1)<=g['gate_spc_cycles']<=2048*profile+4096:raise ValueError('following-rest physical release profile differs')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def volume_updates(report):
    start=report['keyons'][2]['half_cycle'];stop=report['keyons'][3]['half_cycle'];pair=list(report['keyons'][2]['volumes'][0]);previous=pair[:];updates=[]
    for w in report['voice_writes']:
        if start<w['half_cycle']<stop and w['address'] in (0x20,0x21):
            pair[w['address']-0x20]=w['value']
            if w['address']==0x21 and pair!=previous:
                updates.append({'volumes':pair[:],'interval_spc_cycles':(w['half_cycle']-start)/2});previous=pair[:]
    return updates


def compare(candidates,references):
    agree(references)
    keys={f'{c}-{d}-{s}-{a}-{b}' for c in CASES for d in DURATIONS for s in DURATIONS for a in (63,127) for b in (63,127)}
    if not isinstance(candidates,dict) or set(candidates)!=keys:raise ValueError('require complete following-rest native matrix')
    for r in candidates.values():validate(r)
    results=[]
    for ref in references:
        c,a,d,s,b,m=(ref[k] for k in ('case','articulation','boundary_duration','rest_duration','rest_articulation','model'));r=candidates[f'{c}-{d}-{s}-{a}-{b}']
        if ref.get('fixture_sha256')!=hashlib.sha256(build(a,d,s,c,b)).hexdigest():raise ValueError('following-rest owned fixture hash differs')
        gates,rs=native_observation(r)
        if any((g['onset_index'],g['voice'])!=(o['onset_index'],o['voice']) for g,o in zip(gates,ref['note_gates'])):raise ValueError('following-rest release identity differs')
        gd=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,ref['note_gates'])]
        intervals=[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])]
        od=[abs(x-y) for x,y in zip(intervals,ref['onset_intervals_spc_cycles'])]
        if boundary_pitches(r)!=ref['boundary_pitch_writes']:raise ValueError('following-rest pitch write order differs')
        pd=[abs(x-y) for x,y in zip(boundary_offsets(r),ref['boundary_pitch_to_KON_spc_cycles'])]
        updates=volume_updates(r)
        if len(updates)!=1 or updates[0]['volumes']!=[1,1]:raise ValueError('following-rest delayed volume update differs')
        vd=abs(updates[0]['interval_spc_cycles']-ref['voice2_volume_updates'][0]['interval_spc_cycles'])
        if any(v>4096 for v in gd+od+pd+[vd]):raise ValueError('following-rest exceeds retained 4096-cycle allowance')
        results.append({'case':c,'articulation':a,'boundary_duration':d,'rest_duration':s,'rest_articulation':b,'model':m,'absolute_gate_difference_spc_cycles':gd,'absolute_onset_difference_spc_cycles':od,'absolute_pitch_difference_spc_cycles':pd,'absolute_volume_difference_spc_cycles':vd})
    return results


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:refs=json.loads(args.reference.read_text())['reference']
        elif args.trace and args.firmware_dir:
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,s,c,b) for c in CASES for d in DURATIONS for s in DURATIONS for a in (63,127) for b in (63,127) for m in ('sgb','sgb2')]
        else:raise ValueError('provide saved reference or trace and firmware directory')
        candidates={f'{c}-{d}-{s}-{a}-{b}':native(args.probe.resolve(),bank(a,d,s,c,b)) for c in CASES for d in DURATIONS for s in DURATIONS for a in (63,127) for b in (63,127)}
        result={'schema':'gbb-score-follow-rest-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'reference':refs,'native':candidates,'comparisons':compare(candidates,refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
