#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate owned pending-note playback without changing physical timestamps."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_pending import build as program_build
from build_sgb_score_pending_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_pending_reference import agree,contract,expected,ticks,run
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_envelope_reference import validate_trajectories
from check_sgb_score_reselect_reference import validate_writes
from check_sgb_score_peer_reference import native_observation
from check_sgb_score_reverse_reference import boundary_pitches,boundary_offsets
from check_sgb_score_mix_reference import selected
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_gate_reference import PULSES
from check_sgb_score_duet_reference import integer
from schedule_sgb_score import schedule
SCHEMA='gbb-spc-score-pending-v1'


def align(report,data):
    symbolic=schedule(data,int.from_bytes(data[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
    timed=[e for e in symbolic['events'] if e['kind'] in ('note','rest')]
    if report['status']!=2 or report['end_tick']!=symbolic['ticks'] or len(timed)!=len(report['events']) or report['pattern_ticks']!=[p['start_tick'] for p in symbolic['patterns']] or report['pattern_masks']!=[sum(1<<c for c in p['channels']) for p in symbolic['patterns']]:raise ValueError('pending symbolic timeline differs')
    controls={2:[10,127],3:[10,127]};counts={2:0,3:0};song=160;index=0
    for e in symbolic['events']:
        c,k=e['channel'],e['kind']
        if k=='pan':controls[c][0]=e['value']
        elif k=='track_volume':controls[c][1]=e['value']
        elif k=='song_volume':song=e['value']
        elif k=='instrument':counts[c]+=1
        elif k in ('note','rest'):
            actual=report['events'][index];p=report['event_patterns'][index];index+=1
            wanted={'tick':e['tick'],'channel':c,'opcode':0xC9 if k=='rest' else 0x80+e['note'],'duration':e['duration'],'articulation':e['articulation']}
            if any(actual[key]!=value for key,value in wanted.items()) or p!=e['pattern']:raise ValueError('pending raw executed event differs')
            pan,track=controls[c]
            if [actual['pan'],actual['track_volume'],actual['song_volume'],actual['volumes'],actual['instrument_sets']]!=[pan,track,song,selected(pan,track,song,k=='rest'),counts[c]]:raise ValueError('pending logical inheritance differs')
            counts[c]=0
    return timed


def identity(report):
    # This checker qualifies exactly the independently observed owned corpus.
    # Other diagnostics retain their own preceding validators.
    es=report['events'];boundary=[e for e in es if e['tick']==32 and e['channel']==2]
    if len(boundary)!=1:raise ValueError('missing pending boundary event')
    e=boundary[0];a=e['articulation'];d=e['duration']
    rests=[e for e in es if e['tick']==64 and e['channel']==2 and e['opcode']==0xC9]
    c='pending-rest' if e['opcode']==0xC9 else ('long-rest-return' if rests and rests[0]['duration']==16 else 'rest-return' if rests else 'note-return')
    if a not in (63,127) or d not in DURATIONS:raise ValueError('unmeasured pending profile')
    return c,a,d


def validate_metadata(report, *, schema=SCHEMA):
    multi_validate(report,schema=schema,max_events=64,max_ticks=2032,min_events=1,initial_pair=False)
    if report.get('source_unmodified') is not True or report.get('cache_guards_equal') is not True:raise ValueError('pending source/cache guards differ')
    if not integer(report.get('completion_half_cycle'),1,30000000) or not integer(report.get('tempo'),0,255):raise ValueError('invalid pending completion/tempo')
    for name in ('peer_checks','frozen_peer_checks','settled_envelope_checks','settled_gate_frames','settled_peer_nonzero_frames'):
        values=report.get(name)
        if not isinstance(values,list) or len(values)!=2 or any(not integer(v,0,30000000) for v in values):raise ValueError('invalid pending lifecycle observations')
    pcm=report.get('pcm');tail=report.get('second_tail_pcm')
    if not isinstance(pcm,dict) or not isinstance(tail,dict) or not integer(pcm.get('frames'),1,500000) or not integer(pcm.get('peak'),0,32768) or not integer(pcm.get('fnv1a64'),0,2**64-1) or not integer(pcm.get('quiet_tail_frames'),64,pcm['frames']) or not integer(tail.get('frames'),0,pcm['frames']):raise ValueError('invalid pending PCM bounds')
    for key in ('nonzero_frames','left_nonzero_frames','right_nonzero_frames'):
        if not integer(pcm.get(key),0,pcm['frames']):raise ValueError('invalid pending PCM counts')
    for key in ('left_nonzero_frames','right_nonzero_frames'):
        if not integer(tail.get(key),0,tail['frames']):raise ValueError('invalid pending tail counts')
    if report['status']!=2:
        if any(report.get(k)!=[] for k in ('events','event_patterns','pattern_ticks','pattern_masks','keyons','keyoffs','voice_writes','instrument_writes','envelopes')) or pcm['nonzero_frames'] or pcm['peak'] or any(report['frozen_peer_checks']):raise ValueError('rejected pending bank rendered audio')
        return
    if report['tempo']!=96 or not pcm['nonzero_frames'] or pcm['peak']==0:raise ValueError('pending tempo/audio differs')


def validate(report):
    validate_metadata(report)
    if report['status']!=2:return
    for e in report['events']:
        if any(type(e.get(k)) is not int for k in ('articulation','pan','track_volume','song_volume','instrument_sets')) or not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not integer(v,0,127) for v in e['volumes']):raise ValueError('invalid pending raw controls')
    patterns=report.get('event_patterns')
    if not isinstance(patterns,list) or len(patterns)!=len(report['events']) or any(not integer(p,0,3) for p in patterns):raise ValueError('invalid pending event pattern identities')
    if report.get('second_pattern_tick')!=32 or type(report.get('second_pattern_tick')) is not int or not isinstance(report.get('pattern_ticks'),list) or any(not integer(t,0,2032) for t in report['pattern_ticks']) or not isinstance(report.get('pattern_masks'),list) or any(type(m) is not int or m not in (4,8,12) for m in report['pattern_masks']):raise ValueError('invalid pending pattern metadata')
    c,a,d=identity(report);timed=align(report,bank(a,d,c))
    validate_writes(report);validate_trajectories(report,max_events=64,clipped_peer=True)
    writes=report.get('voice_writes')
    if not isinstance(writes,list) or len(writes)>256:raise ValueError('invalid pending DSP write count')
    previous=0
    for w in writes:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,report['completion_half_cycle']) or not integer(w.get('address'),0x20,0x33) or w['address'] not in (0x20,0x21,0x22,0x23,0x30,0x31,0x32,0x33) or not integer(w.get('value'),0,255):raise ValueError('invalid pending DSP write')
        previous=w['half_cycle']
    consumed=0
    for i,(e,symbolic) in enumerate(zip(report['events'],timed)):
        stop=report['events'][i+1]['half_cycle'] if i+1<len(timed) else report['completion_half_cycle']
        actual=[w for w in writes if e['half_cycle']<=w['half_cycle']<stop];wanted=[]
        if e['opcode']!=0xC9:
            pitch=PITCH[e['opcode']];base=16*e['channel']
            wanted=[] if symbolic.get('boundary_event') else [(base,e['volumes'][0]),(base+1,e['volumes'][1])]
            wanted.extend([(base+2,pitch&255),(base+3,pitch>>8)])
        if [(w['address'],w['value']) for w in actual]!=wanted:raise ValueError('pending actual VOL/pitch writes differ')
        consumed+=len(actual)
    if consumed!=len(writes):raise ValueError('pending DSP write outside executed event')
    for name in ('keyons','keyoffs'):
        edges=report.get(name)
        if not isinstance(edges,list) or len(edges)>64:raise ValueError('invalid pending edge count')
        previous=0
        for edge in edges:
            if not isinstance(edge,dict) or not integer(edge.get('half_cycle'),previous+1,report['completion_half_cycle']) or not integer(edge.get('tick'),0,report['end_tick']) or any(type(edge.get(k)) is not int for k in ('mask','affected_mask','held_mask','cause')) or edge['mask'] not in (4,8,12) or edge['affected_mask'] not in (4,8,12) or edge['held_mask'] not in (0,4,8,12) or edge['cause']!=(0 if name=='keyons' else 1):raise ValueError('invalid pending physical edge')
            previous=edge['half_cycle']
            for key,size,limit in (('pitches',2,0x3FFF),('pending_pulses',2,58)):
                if not isinstance(edge.get(key),list) or len(edge[key])!=size or any(not integer(v,0,limit) for v in edge[key]):raise ValueError('invalid pending edge setup')
            if not isinstance(edge.get('volumes'),list) or len(edge['volumes'])!=2 or any(not isinstance(v,list) or len(v)!=2 or any(not integer(x,0,127) for x in v) for v in edge['volumes']):raise ValueError('invalid pending live volumes')
    if len(report['keyons'])!=8:raise ValueError('pending KON count differs')
    for edge,target,tick in zip(report['keyons'],expected(c),ticks(c)):
        group=[e for e in report['events'] if e['tick']==tick];affected=sum(1<<e['channel'] for e in group if not (e['tick']==32 and e['channel']==2 and e['opcode']==0xC9))
        if edge['tick']!=tick or edge['mask']!=target['mask'] or edge['affected_mask']!=affected or edge['held_mask']&edge['mask'] or not 0<edge['half_cycle']-group[-1]['half_cycle']<=4096:raise ValueError('pending KON grouping/timing differs')
        for e in group:
            if e['opcode']!=0xC9 and edge['pending_pulses'][e['channel']-2]!=PULSES[(96,e['articulation'],e['duration'])]:raise ValueError('pending KON gate profile differs')
        for v in target['voices']:
            if edge['pitches'][v['voice']-2]!=v['pitch'] or edge['volumes'][v['voice']-2]!=v['volumes']:raise ValueError('pending physical KON pitch/volume differs')
    held=12
    for half,is_on,edge in sorted([(e['half_cycle'],True,e) for e in report['keyons']]+[(e['half_cycle'],False,e) for e in report['keyoffs']]):
        held=(held & ~edge['mask']) if is_on else (held | edge['mask'])
        if edge['held_mask']!=held or (not is_on and edge['affected_mask']!=edge['mask']):raise ValueError('pending physical held KOF mask differs')
    gates,retriggers=native_observation(report)
    if len(gates)!=(13 if c in ('rest-return','long-rest-return') else 12) or len(retriggers)!=(1 if c=='note-return' else 0):raise ValueError('pending physical release/retrigger count differs')
    for g in gates:
        if g['cause']!=1:raise ValueError('scheduler invented pending KOF')
        if (g['onset_index'],g['voice'])==(2,2):
            profile=PULSES[(96,a,16 if c=='long-rest-return' else 8)]
            low,high=168000+profile*2048-4096,184000+profile*2048+4096
        else:low,high=2048*(PULSES[(96,a,16)]-1),2048*PULSES[(96,a,16)]+4096
        if not low<=g['gate_spc_cycles']<=high:raise ValueError('pending physical gate outside measured profile')
    if c!='pending-rest' and report['frozen_peer_checks'][0]==0:raise ValueError('pending held voice not observed frozen')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def volume_updates(report):
    start=report['keyons'][2]['half_cycle'];stop=report['keyons'][4]['half_cycle'];pair=list(report['keyons'][2]['volumes'][0]);previous=pair[:];updates=[]
    for w in report['voice_writes']:
        if start<w['half_cycle']<stop and w['address'] in (0x20,0x21):
            pair[w['address']-0x20]=w['value']
            if w['address']==0x21 and pair!=previous:
                updates.append({'volumes':pair[:],'interval_spc_cycles':(w['half_cycle']-start)/2});previous=pair[:]
    return updates


def compare(candidates,references):
    agree(references)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{d}-{a}' for c in CASES for d in DURATIONS for a in (63,127)}:raise ValueError('require complete pending native matrix')
    for r in candidates.values():validate(r)
    results=[]
    for ref in references:
        c,a,d,m=(ref[k] for k in ('case','articulation','pending_duration','model'));r=candidates[f'{c}-{d}-{a}']
        if ref.get('fixture_sha256')!=hashlib.sha256(build(a,d,c)).hexdigest():raise ValueError('pending reference fixture hash differs')
        gates,retriggers=native_observation(r)
        if any((g['onset_index'],g['voice'])!=(o['onset_index'],o['voice']) for g,o in zip(gates,ref['note_gates'])):raise ValueError('pending release identities differ')
        gd=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,ref['note_gates'])]
        intervals=[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])]
        od=[abs(x-y) for x,y in zip(intervals,ref['onset_intervals_spc_cycles'])]
        if boundary_pitches(r)!=ref['boundary_pitch_writes']:raise ValueError('pending boundary pitch order differs')
        pd=[abs(x-y) for x,y in zip(boundary_offsets(r),ref['boundary_pitch_to_KON_spc_cycles'])]
        updates=volume_updates(r)
        if len(updates)!=1 or updates[0]['volumes']!=[1,1]:raise ValueError('pending delayed volume update differs')
        vd=abs(updates[0]['interval_spc_cycles']-ref['voice2_volume_updates'][0]['interval_spc_cycles'])
        rd=[]
        if len(retriggers)!=len(ref['unreleased_retriggers']):raise ValueError('pending retrigger count differs')
        for x,y in zip(retriggers,ref['unreleased_retriggers']):
            if any(x[k]!=y[k] for k in ('voice','onset_index','previous_onset_index')):raise ValueError('pending retrigger identity differs')
            rd.append(abs(x['interval_spc_cycles']-y['interval_spc_cycles']))
        if any(v>4096 for v in gd+od+pd+rd+[vd]):raise ValueError('pending exceeds retained 4096-cycle allowance')
        results.append({'case':c,'articulation':a,'pending_duration':d,'model':m,'absolute_gate_difference_spc_cycles':gd,'absolute_onset_difference_spc_cycles':od,'absolute_pitch_difference_spc_cycles':pd,'absolute_volume_difference_spc_cycles':vd,'absolute_retrigger_difference_spc_cycles':rd})
    return results


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            refs=json.loads(args.reference.read_text())['reference']
        elif args.trace and args.firmware_dir:
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for d in DURATIONS for a in (63,127) for m in ('sgb','sgb2')]
        else:raise ValueError('provide saved reference or trace and firmware directory')
        candidates={f'{c}-{d}-{a}':native(args.probe.resolve(),bank(a,d,c)) for c in CASES for d in DURATIONS for a in (63,127)}
        result={'schema':'gbb-score-pending-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'reference':refs,'native':candidates,'comparisons':compare(candidates,refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
