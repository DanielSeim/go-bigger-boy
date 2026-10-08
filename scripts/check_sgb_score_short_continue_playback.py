#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate guarded short continuation without altering physical timestamps."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_short_continue import build as program_build
from build_sgb_score_short_continue_fixture import bank,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_short_continue_reference import expected,agree,run,SCHEMA as REF_SCHEMA
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_pending_playback import align,validate_metadata
from check_sgb_score_envelope_reference import validate_trajectories
from check_sgb_score_reselect_reference import validate_writes
from check_sgb_score_final_playback import observation,control_writes
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_gate_reference import PULSES
from check_sgb_score_duet_reference import integer
SCHEMA='gbb-spc-score-short-continue-v1'


def identity(r,*,short_pair=False):
    es=r['events']
    if len(es)!=10:raise ValueError('short continuation requires ten records')
    c='continue-rest' if es[8]['opcode']==0xC9 else 'continue-note';a,d=es[8]['articulation'],es[8]['duration']
    if type(a) is not int or a not in ARTICULATIONS or type(d) is not int or d not in ((4,) if short_pair else DURATIONS):raise ValueError('unmeasured short continuation')
    return c,a,d


def validate(r,*,short_pair=False,fixture_bank=bank):
    multi_validate(r,schema=SCHEMA,max_events=64,max_ticks=2032,min_events=1,initial_pair=False)
    validate_metadata(r,schema=SCHEMA)
    modes=('final_return_mode','direct_return_mode','short_return_mode','short_continue_mode')
    if r['status']!=2:
        if r.get('key_writes')!=[] or any(not integer(r.get(k),0,0) for k in (*modes,'final_mode','final_observation_half_cycles')):raise ValueError('rejected short continuation wrote keys')
        return
    if any(not integer(r.get(k),1,1) for k in modes) or not integer(r.get('final_mode'),0,0) or not integer(r.get('final_peer_stop'),0,0) or not integer(r.get('final_observation_half_cycles'),600000,600000):raise ValueError('short continuation mode/tail differs')
    c,a,d=identity(r,short_pair=short_pair);align(r,fixture_bank(a,d,c));validate_writes(r)
    if r['pattern_ticks']!=[0,32,64] or r['pattern_masks']!=[12,8,12] or r['event_patterns']!=[0]*4+[1]*2+[2]*4 or r['end_tick']!=68+d or r['second_pattern_tick']!=32:raise ValueError('short continuation geometry differs')
    if [e['articulation'] for e in r['events']]!=[127]*6+[a]*4:raise ValueError('short continuation articulation differs')
    writes=r.get('voice_writes');previous=0
    if not isinstance(writes,list) or len(writes)>256:raise ValueError('short continuation DSP write bound')
    for w in writes:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,r['completion_half_cycle']) or not integer(w.get('address'),32,51) or w['address'] not in (32,33,34,35,48,49,50,51) or not integer(w.get('value'),0,255):raise ValueError('invalid short continuation DSP write')
        previous=w['half_cycle']
    count=0
    for i,e in enumerate(r['events']):
        end=r['events'][i+1]['half_cycle'] if i<9 else r['completion_half_cycle']
        actual=[w for w in writes if e['half_cycle']<=w['half_cycle']<end];wanted=[]
        if e['opcode']!=0xC9:
            b=16*e['channel'];p=PITCH[e['opcode']];vol=[(b,e['volumes'][0]),(b+1,e['volumes'][1])];pitch=[(b+2,p&255),(b+3,p>>8)]
            wanted=pitch+vol if i in (6,8) else vol+pitch
        if [(w['address'],w['value']) for w in actual]!=wanted:raise ValueError('short continuation pitch/VOL order differs')
        count+=len(actual)
    if count!=len(writes):raise ValueError('unbound short continuation DSP writes')
    for name in ('keyons','keyoffs'):
        previous=0
        if not isinstance(r.get(name),list) or len(r[name])>64:raise ValueError('invalid short continuation edges')
        for e in r[name]:
            if not isinstance(e,dict) or not integer(e.get('half_cycle'),previous+1,r['completion_half_cycle']) or not integer(e.get('tick'),0,r['end_tick']) or any(type(e.get(k)) is not int for k in ('mask','affected_mask','held_mask','cause')) or e['mask'] not in (4,8,12) or e['affected_mask'] not in (4,8,12) or e['held_mask'] not in (0,4,8,12) or e['cause']!=(0 if name=='keyons' else 1):raise ValueError('invalid short continuation physical edge')
            previous=e['half_cycle']
            for k,limit in (('pitches',0x3FFF),('pending_pulses',58)):
                if not isinstance(e.get(k),list) or len(e[k])!=2 or any(not integer(v,0,limit) for v in e[k]):raise ValueError('invalid short continuation physical setup')
            if not isinstance(e.get('volumes'),list) or len(e['volumes'])!=2 or any(not isinstance(p,list) or len(p)!=2 or any(not integer(v,0,127) for v in p) for p in e['volumes']):raise ValueError('invalid short continuation live volume')
    wanted=expected(c);ticks=[0,16,32,48,64]+([68] if c=='continue-note' else [])
    if len(r['keyons'])!=len(wanted):raise ValueError('short continuation onset count differs')
    for i,(e,w,t) in enumerate(zip(r['keyons'],wanted,ticks)):
        group=[x for x in r['events'] if x['tick']==t]
        if e['tick']!=t or e['mask']!=w['mask'] or e['affected_mask']!=(12 if i in (4,5) else w['mask']) or e['held_mask']&e['mask'] or not 0<e['half_cycle']-group[-1]['half_cycle']<=4096:raise ValueError('short continuation KON group differs')
        for v in w['voices']:
            channel=v['voice']-2;pulses=5 if i==4 else (5 if short_pair else PULSES[(96,a,d)]) if i==5 else PULSES[(96,127,24 if i==1 and channel==0 else 16)]
            if e['pitches'][channel]!=v['pitch'] or e['volumes'][channel]!=v['volumes'] or e['pending_pulses'][channel]!=pulses:raise ValueError('short continuation live setup/gate differs')
        if i in (4,5) and e['held_mask']!=0:raise ValueError('short continuation retained inactive KOF')
    keys=r.get('key_writes');previous=0
    if not isinstance(keys,list) or len(keys)>1024:raise ValueError('short continuation key write bound')
    active=0;ons=[];offs=[]
    for w in keys:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,r['completion_half_cycle']) or not integer(w.get('address'),76,92) or w['address'] not in (76,92) or not integer(w.get('value'),0,255) or w['value'] not in (0,4,8,12,255) or w['address']==76 and w['value']==255:raise ValueError('invalid short continuation key write')
        previous=w['half_cycle'];mask=w['value']&12
        if w['address']==76 and mask:ons.append((previous,mask));active|=mask
        elif w['address']==92:
            released=active&mask
            if released:offs.append((previous,released));active&=~released
    if ons!=[(e['half_cycle'],e['mask']) for e in r['keyons']] or offs!=[(e['half_cycle'],e['mask']) for e in r['keyoffs']] or active:raise ValueError('short continuation physical ledger differs from raw keys')
    gates,held=observation(r,allow_retrigger=True)
    identities=[(0,2),(0,3),(1,3),(2,3),(3,3),(4,2)]+([(5,2)] if c=='continue-note' else [])
    if held or [(g['onset_index'],g['voice']) for g in gates]!=identities or any(g['cause']!=1 for g in gates):raise ValueError('short continuation timer release ledger differs')
    for g in gates:
        i,v=g['onset_index'],g['voice'];pulses=5 if i==4 else (5 if short_pair else PULSES[(96,a,d)]) if i==5 else PULSES[(96,127,16)]
        if not 2048*(pulses-1)<=g['gate_spc_cycles']<=2048*pulses+4096:raise ValueError('short continuation timer gate differs')
    controls=control_writes(r)
    if [(w['address'],w['value']) for w in controls]!=[(92,255),(92,0),(76,0)]:raise ValueError('short continuation final pulse differs')
    if any(e['half_cycle']>=controls[0]['half_cycle'] for e in r['keyoffs']):raise ValueError('short continuation did not release before stop')
    for e in r['keyons'][4:]:
        ws=[w for w in keys if w['half_cycle']<=e['half_cycle']][-3:]
        if [(w['address'],w['value']) for w in ws]!=[(92,0),(92,0),(76,4)]:raise ValueError('short continuation KON pulse differs')
    if any(w['value'] for w in keys[keys.index(controls[-1])+1:]):raise ValueError('short continuation post-stop control differs')
    validate_trajectories(r,max_events=64,clipped_peer=True)
    old,new=r['envelopes'][2],r['envelopes'][6]
    if old['off_half_cycle'] or old['retrigger_half_cycle']!=new['on_half_cycle'] or not new['release_zero'] or not r['frozen_peer_checks'][0] or r['frozen_peer_checks'][1]:raise ValueError('short continuation frozen/retrigger lifecycle differs')
    if c=='continue-note' and (not r['envelopes'][-1]['release_zero'] or new['off_half_cycle']>=r['keyons'][-1]['half_cycle']):raise ValueError('short continuation progression envelope differs')
    tail=r.get('final_tail_pcm')
    if not isinstance(tail,dict) or not integer(tail.get('frames'),1,500000) or not integer(tail.get('nonzero_frames'),0,0) or not integer(tail.get('peak'),0,0) or not integer(tail.get('quiet_tail_frames'),64,tail['frames']) or not integer(tail.get('fnv1a64'),0,2**64-1) or any(not integer(r.get(k),0,0) for k in ('final_env_start','final_env_end')):raise ValueError('short continuation final audio not silent')


def native(probe,data,tempo=96):return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,refs,*,short_pair=False,validator=validate,reference_agreement=agree):
    reference_agreement(refs)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{a}-{d}' for c in CASES for a in ARTICULATIONS for d in ((4,) if short_pair else DURATIONS)}:raise ValueError('incomplete short continuation native matrix')
    results=[]
    for ref in refs:
        c,a,d,m=(ref[k] for k in ('case','return_articulation','continuation_duration','model'));r=candidates[f'{c}-{a}-{d}'];validator(r)
        if identity(r,short_pair=short_pair)!=(c,a,d):raise ValueError('short continuation identity differs')
        gates,held=observation(r,allow_retrigger=True);controls=control_writes(r);stop=controls[0]['half_cycle'];ret=r['keyons'][4]['half_cycle'];following=r['keyons'][5]['half_cycle'] if c=='continue-note' else stop
        keys=r['key_writes'];rv=[w for w in r['voice_writes'] if r['keyons'][3]['half_cycle']<w['half_cycle']<=ret];cv=[w for w in r['voice_writes'] if ret<w['half_cycle']<stop]
        rc=[w for w in keys if w['half_cycle']<=ret][-3:];cc=[w for w in keys if w['half_cycle']<=following][-3:] if c=='continue-note' else []
        values={'note_gates':[g['gate_spc_cycles'] for g in gates], 'onset_intervals_spc_cycles':[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])],
            'return_control_offsets_spc_cycles':[(w['half_cycle']-ret)/2 for w in rc], 'return_voice_offsets_spc_cycles':[(w['half_cycle']-ret)/2 for w in rv],
            'continuation_control_offsets_spc_cycles':[(w['half_cycle']-following)/2 for w in cc], 'continuation_voice_offsets_spc_cycles':[(w['half_cycle']-following)/2 for w in cv],
            'final_control_offsets_spc_cycles':[(w['half_cycle']-stop)/2 for w in controls], 'unreleased_retriggers':[(r['keyons'][4]['half_cycle']-r['keyons'][1]['half_cycle'])/2], 'final_stop_interval_spc_cycles':[(stop-ret)/2]}
        diffs={}
        for field,vs in values.items():
            target=ref[field]
            if field=='note_gates':target=[g['gate_spc_cycles'] for g in target]
            elif field=='unreleased_retriggers':target=[g['interval_spc_cycles'] for g in target]
            elif field=='final_stop_interval_spc_cycles':target=[target]
            if len(vs)!=len(target):raise ValueError('short continuation timing vector differs')
            diffs[field]=[abs(v-w) for v,w in zip(vs,target)]
        if held or any(v>4096 for vs in diffs.values() for v in vs):raise ValueError('short continuation exceeds retained timing allowance')
        results.append({'case':c,'return_articulation':a,'continuation_duration':d,'model':m,'absolute_differences_spc_cycles':diffs})
    return results


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            if args.reference.stat().st_size>1024*1024:raise ValueError('short continuation reference exceeds JSON bound')
            loaded=json.loads(args.reference.read_text())
            if loaded.get('schema')!=REF_SCHEMA or loaded.get('qualification') is not False or loaded.get('playback') is not False:raise ValueError('invalid short continuation reference artifact')
            refs=loaded['reference']
        else:
            if not args.trace or not args.firmware_dir:p.error('provide --reference or --trace and --firmware-dir')
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for a in ARTICULATIONS for d in DURATIONS for m in ('sgb','sgb2')]
        candidates={f'{c}-{a}-{d}':native(args.probe.resolve(),bank(a,d,c)) for c in CASES for a in ARTICULATIONS for d in DURATIONS}
        result={'schema':'gbb-score-short-continue-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'native':candidates,'reference':refs,'comparisons':compare(candidates,refs),'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
