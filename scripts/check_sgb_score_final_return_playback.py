#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate returning-rest release, pending final KON and bounded held audio."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_final_return import build as program_build
from build_sgb_score_final_return_fixture import bank,build,CASES,RESTS,DURATIONS
from check_sgb_score_final_return_reference import expected,contract,agree,run,SCHEMA as REF_SCHEMA
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_pending_playback import align,validate_metadata
from check_sgb_score_final_playback import control_writes,observation
from check_sgb_score_envelope_reference import validate_trajectories
from check_sgb_score_reselect_reference import validate_writes
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_gate_reference import PULSES
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_duet_reference import integer
SCHEMA='gbb-spc-score-final-return-v1'


def identity(r,*,return_art=None):
    es=r['events']
    if len(es)!=9:raise ValueError('final return requires nine raw records')
    e=es[-1];c='return-rest' if e['opcode']==0xC9 else 'return-note';a=e['articulation'];d=e['duration'];s=es[6]['duration']
    if type(a) is not int or a not in (63,127) or type(d) is not int or d not in DURATIONS or type(s) is not int or s not in RESTS:raise ValueError('unmeasured final return profile')
    if return_art is not None:
        if return_art!=63 or a!=63 or es[0]['articulation']!=127:raise ValueError('invalid mixed returning articulation')
        a=127
    return c,a,d,s


def validate(r,*,return_art=None,fixture_bank=bank):
    multi_validate(r,schema=SCHEMA,max_events=64,max_ticks=2032,min_events=1,initial_pair=False,terminal_event=True)
    if r['status']!=2:
        validate_metadata(r,schema=SCHEMA)
        if r.get('key_writes')!=[] or r.get('final_return_mode')!=0 or type(r.get('final_return_mode')) is not int:raise ValueError('rejected final return bank wrote keys')
        return
    if r.get('source_unmodified') is not True or r.get('cache_guards_equal') is not True or not integer(r.get('completion_half_cycle'),1,30000000) or not integer(r.get('tempo'),96,96):raise ValueError('invalid final return source/cache/clock')
    for k,v in (('final_mode',1),('final_return_mode',1),('final_peer_stop',0),('final_observation_half_cycles',600000)):
        if not integer(r.get(k),v,v):raise ValueError('invalid final return mode/tail')
    c,a,d,s=identity(r,return_art=return_art)
    if r.get('pattern_ticks')!=[0,32,64] or r.get('pattern_masks')!=[12,8,12] or r.get('event_patterns')!=[0]*4+[1]*2+[2]*3 or not integer(r.get('end_tick'),64+s,64+s) or not integer(r.get('second_pattern_tick'),32,32):raise ValueError('final return pattern geometry differs')
    for k in ('pattern_ticks','pattern_masks','event_patterns'):
        if any(type(v) is not int for v in r[k]):raise ValueError('invalid final return pattern types')
    for i,e in enumerate(r['events']):
        wanted_art=return_art if return_art is not None and i>=6 else a
        if any(type(e.get(k)) is not int for k in ('articulation','pan','track_volume','song_volume','instrument_sets')) or e['articulation']!=wanted_art or not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not integer(v,0,127) for v in e['volumes']):raise ValueError('invalid final return raw controls')
    timed=align(r,fixture_bank(a,d,c,s));validate_writes(r)
    for name in ('keyons','keyoffs'):
        previous=0
        if not isinstance(r.get(name),list) or len(r[name])>64:raise ValueError('invalid final return edges')
        for e in r[name]:
            if not isinstance(e,dict) or not integer(e.get('half_cycle'),previous+1,r['completion_half_cycle']) or not integer(e.get('tick'),0,r['end_tick']) or any(type(e.get(k)) is not int for k in ('mask','affected_mask','held_mask','cause')) or e['mask'] not in (4,8,12) or e['affected_mask'] not in (4,8,12) or e['held_mask'] not in (0,4,8,12) or e['cause']!=(0 if name=='keyons' else 1):raise ValueError('invalid final return physical edge')
            previous=e['half_cycle']
            for k,limit in (('pitches',0x3FFF),('pending_pulses',58)):
                if not isinstance(e.get(k),list) or len(e[k])!=2 or any(not integer(v,0,limit) for v in e[k]):raise ValueError('invalid final return physical setup')
            if not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not isinstance(p,list) or len(p)!=2 or any(not integer(v,0,127) for v in p) for p in e['volumes']):raise ValueError('invalid final return live volume')
    if len(r['keyons'])!=len(expected(c)):raise ValueError('final return KON count differs')
    for i,(e,w,tick) in enumerate(zip(r['keyons'],expected(c),[0,16,32,48,64+s])):
        group=[x for x in r['events'] if x['tick']==tick]
        if e['tick']!=tick or e['mask']!=w['mask'] or e['affected_mask']!=w['mask'] or e['held_mask']&e['mask'] or not 0<e['half_cycle']-group[-1]['half_cycle']<=4096:raise ValueError('final return KON grouping differs')
        for v in w['voices']:
            pulses=0 if i==4 else PULSES[(96,a,24 if i==1 and v['voice']==2 else 16)]
            if e['pitches'][v['voice']-2]!=v['pitch'] or e['volumes'][v['voice']-2]!=v['volumes'] or e['pending_pulses'][v['voice']-2]!=pulses:raise ValueError('final return physical note/profile differs')
    writes=r.get('voice_writes');previous=0
    if not isinstance(writes,list) or len(writes)>256:raise ValueError('invalid final return VOL/pitch writes')
    for w in writes:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,r['completion_half_cycle']) or type(w.get('address')) is not int or w['address'] not in (32,33,34,35,48,49,50,51) or not integer(w.get('value'),0,255):raise ValueError('invalid final return voice write')
        previous=w['half_cycle']
    consumed=0
    for i,(e,t) in enumerate(zip(r['events'],timed)):
        stop=r['events'][i+1]['half_cycle'] if i+1<len(timed) else r['completion_half_cycle'];actual=[w for w in writes if e['half_cycle']<=w['half_cycle']<stop];wanted=[]
        if e['opcode']!=0xC9:
            base=16*e['channel'];p=PITCH[e['opcode']]
            if not t.get('boundary_event'):wanted.extend([(base,e['volumes'][0]),(base+1,e['volumes'][1])])
            wanted.extend([(base+2,p&255),(base+3,p>>8)])
        if [(w['address'],w['value']) for w in actual]!=wanted:raise ValueError('final return actual VOL/pitch differs')
        consumed+=len(actual)
    if consumed!=len(writes):raise ValueError('unbound final return voice write')
    keys=r.get('key_writes');previous=0
    if not isinstance(keys,list) or len(keys)>1024:raise ValueError('invalid final return key-write bound')
    kon=0;kof=12;on=0;off=0
    for w in keys:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,r['completion_half_cycle']) or type(w.get('address')) is not int or w['address'] not in (76,92) or type(w.get('value')) is not int or w['value'] not in (0,4,8,12,255) or (w['address']==76 and w['value']==255):raise ValueError('invalid final return key write')
        previous=w['half_cycle']
        if w['address']==76:
            if w['value'] and not kon:
                if on>=len(r['keyons']) or (w['half_cycle'],w['value'])!=(r['keyons'][on]['half_cycle'],r['keyons'][on]['mask']):raise ValueError('final return KON not bound to write')
                on+=1
            kon=w['value']
        else:
            asserted=(w['value']&~kof)&12;kof=w['value']
            if asserted:
                if off>=len(r['keyoffs']) or (w['half_cycle'],asserted,kof)!=(r['keyoffs'][off]['half_cycle'],r['keyoffs'][off]['mask'],r['keyoffs'][off]['held_mask']):raise ValueError('final return KOF not bound to write')
                off+=1
    if (on,off)!=(len(r['keyons']),len(r['keyoffs'])) or kon or kof:raise ValueError('final return key ledger incomplete')
    controls=control_writes(r)
    if [(w['address'],w['value']) for w in controls]!=[(92,255),(92,0),(76,4 if c=='return-note' else 0)] or any(not 0<=w['half_cycle']-controls[0]['half_cycle']<=8192 for w in controls):raise ValueError('final return stop pulse differs')
    validate_trajectories(r,max_events=64,clipped_peer=True,held_final=(2,) if c=='return-note' else ())
    gates,held=observation(r)
    if [(g['onset_index'],g['voice']) for g in gates]!=[(0,2),(0,3),(1,2),(1,3),(2,3),(3,3)] or held!=([2] if c=='return-note' else []):raise ValueError('final return release/held identities differ')
    for g in gates:
        if g['cause']!=1:raise ValueError('final return substituted scheduler release')
        if (g['onset_index'],g['voice'])==(1,2) and a==127:
            pulses=5 if s==4 else (9 if return_art==63 else 15)
            # Retain raw timestamps. Only this local profile check anchors
            # to the observed returning rest; the exported gate stays raw.
            rest_half=r['events'][6]['half_cycle'];release=next(e['half_cycle'] for e in r['keyoffs'] if e['mask']&4 and e['half_cycle']>r['keyons'][1]['half_cycle'])
            if not pulses*2048-2048<=(release-rest_half)/2<=pulses*2048+4096:raise ValueError('returning rest did not rearm measured gate')
        else:
            pulses=PULSES[(96,a,24 if (g['onset_index'],g['voice'])==(1,2) else 16)]
            if not 2048*(pulses-1)<=g['gate_spc_cycles']<=2048*pulses+4096:raise ValueError('final return preceding timer profile differs')
    for name in ('peer_checks','frozen_peer_checks','settled_envelope_checks','settled_gate_frames','settled_peer_nonzero_frames'):
        if not isinstance(r.get(name),list) or len(r[name])!=2 or any(not integer(v,0,30000000) for v in r[name]):raise ValueError('invalid final return lifecycle counters')
    if bool(r['frozen_peer_checks'][0])!=(a==127) or r['frozen_peer_checks'][1]:raise ValueError('final return inactive freeze observations differ')
    pcm=r.get('pcm');tail=r.get('second_tail_pcm');final=r.get('final_tail_pcm')
    if not isinstance(pcm,dict) or not isinstance(tail,dict) or not isinstance(final,dict) or not integer(pcm.get('frames'),1,500000) or not integer(pcm.get('peak'),1,32768) or not integer(pcm.get('fnv1a64'),0,2**64-1) or not integer(pcm.get('quiet_tail_frames'),0 if c=='return-note' else 64,pcm['frames']) or not integer(tail.get('frames'),1,pcm['frames']):raise ValueError('invalid final return PCM bounds')
    for k in ('nonzero_frames','left_nonzero_frames','right_nonzero_frames'):
        if not integer(pcm.get(k),1,pcm['frames']):raise ValueError('invalid final return PCM activity')
    if not integer(final.get('frames'),1,pcm['frames']) or not integer(final.get('fnv1a64'),0,2**64-1) or not integer(final.get('nonzero_frames'),1 if c=='return-note' else 0,final['frames'] if c=='return-note' else 0) or not integer(final.get('peak'),1 if c=='return-note' else 0,32768 if c=='return-note' else 0) or not integer(final.get('quiet_tail_frames'),0 if c=='return-note' else 64,63 if c=='return-note' else final['frames']):raise ValueError('final return held/rest PCM tail differs')
    for k in ('final_env_start','final_env_end'):
        if not integer(r.get(k),1 if c=='return-note' else 0,127 if c=='return-note' else 0):raise ValueError('final return terminal envelope differs')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,refs):
    agree(refs)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{s}-{d}-{a}' for c in CASES for s in RESTS for d in DURATIONS for a in (63,127)}:raise ValueError('require complete final return native matrix')
    result=[]
    for ref in refs:
        c,a,d,s,m=(ref[k] for k in ('case','articulation','boundary_duration','return_rest','model'));r=candidates[f'{c}-{s}-{d}-{a}'];validate(r)
        if ref.get('fixture_sha256')!=hashlib.sha256(build(a,d,c,s)).hexdigest():raise ValueError('final return owned cartridge hash differs')
        gates,held=observation(r);controls=control_writes(r);stop=controls[0]['half_cycle']
        gd=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,ref['note_gates'])]
        intervals=[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])];od=[abs(x-y) for x,y in zip(intervals,ref['onset_intervals_spc_cycles'])]
        sd=abs((stop-r['keyons'][3]['half_cycle'])/2-ref['final_stop_interval_spc_cycles'])
        pd=[abs(x-y) for x,y in zip([(stop-w['half_cycle'])/2 for w in r['voice_writes'] if w['address']==35 and w['half_cycle']>r['keyons'][3]['half_cycle']],ref['boundary_pitch_to_stop_spc_cycles'])]
        cd=[abs((w['half_cycle']-stop)/2-o) for w,o in zip(controls,ref['final_control_offsets_spc_cycles'])]
        if len(pd)!=len(ref['boundary_pitch_writes']) or held!=ref['unreleased_final_voices'] or any(v>4096 for v in gd+od+pd+cd+[sd]):raise ValueError('final return exceeds retained timing allowance')
        result.append({'case':c,'articulation':a,'boundary_duration':d,'return_rest':s,'model':m,'absolute_gate_difference_spc_cycles':gd,'absolute_onset_difference_spc_cycles':od,'absolute_pitch_difference_spc_cycles':pd,'absolute_control_difference_spc_cycles':cd,'absolute_stop_difference_spc_cycles':sd})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            if args.reference.stat().st_size>1024*1024:raise ValueError('final return JSON reference exceeds bound')
            loaded=json.loads(args.reference.read_text())
            if loaded.get('schema')!=REF_SCHEMA or loaded.get('qualification') is not False or loaded.get('playback') is not False:raise ValueError('invalid final return observation artifact')
            refs=loaded['reference']
        else:
            if not args.trace or not args.firmware_dir:p.error('provide --reference or --trace and --firmware-dir')
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c,s) for c in CASES for s in RESTS for d in DURATIONS for a in (63,127) for m in ('sgb','sgb2')]
        candidates={f'{c}-{s}-{d}-{a}':native(args.probe.resolve(),bank(a,d,c,s)) for c in CASES for s in RESTS for d in DURATIONS for a in (63,127)}
        result={'schema':'gbb-score-final-return-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'native':candidates,'reference':refs,'comparisons':compare(candidates,refs),'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
