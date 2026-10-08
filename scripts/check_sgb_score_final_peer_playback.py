#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bind measured final peer releases to actual native writes and lifecycle."""
import argparse,copy,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_final_peer import build as program_build
from build_sgb_score_final_peer_fixture import bank,build,expected,CASES
from check_sgb_score_final_peer_reference import contract,agree,run,SCHEMA as REF_SCHEMA
from check_sgb_score_peer_reference import validate as peer_validate,align as peer_align,SCHEMA as PEER_SCHEMA
from check_sgb_score_final_playback import control_writes,observation
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_duet_reference import integer

SCHEMA='gbb-spc-score-final-peer-v1'


def peer_report(r):
    result=copy.deepcopy(r);result['schema']=PEER_SCHEMA
    # The existing validator describes only voices 2/3. Preserve raw FF in
    # the public report and verify it separately against accepted DSP writes.
    for edge in result['keyoffs']:
        if edge['held_mask']==255:edge['held_mask']=12
    return result


def validate(r):
    if not isinstance(r,dict) or r.get('schema')!=SCHEMA:raise ValueError('invalid final peer schema')
    if not integer(r.get('final_peer_stop'),0,1) or r.get('final_mode')!=0 or type(r.get('final_mode')) is not int:
        raise ValueError('invalid final peer completion selection')
    keys=r.get('key_writes');previous=0
    if not isinstance(keys,list) or len(keys)>1024:raise ValueError('invalid final peer key-write bound')
    for w in keys:
        if not isinstance(w,dict) or not integer(w.get('half_cycle'),previous+1,r['completion_half_cycle']) or type(w.get('address')) is not int or w['address'] not in (76,92) or type(w.get('value')) is not int or w['value'] not in (0,4,8,12,255) or (w['address']==76 and w['value']==255):
            raise ValueError('invalid final peer key write')
        previous=w['half_cycle']
    # Validate every existing score, source/cache, physical envelope and PCM
    # contract before admitting the new stop sequence.
    peer_validate(peer_report(r))
    if r['status']!=2:
        if r['final_peer_stop'] or keys:raise ValueError('rejected final peer score wrote DSP keys')
        return
    if r['final_peer_stop']!=1:raise ValueError('score is outside measured final peer completion')
    if r['tempo']!=96 or type(r['tempo']) is not int:raise ValueError('unmeasured final peer tempo')
    for edge in r['keyons']+r['keyoffs']:
        if not integer(edge['held_mask'],0,255) or edge['held_mask'] not in (0,4,8,12,255):raise ValueError('invalid raw final peer held register')
    # Bind the full accepted register ledger to every actual edge. No new
    # release is inferred when a previously asserted bit is written again.
    kon=0;kof=12;on=0;off=0
    for w in keys:
        if w['address']==76:
            if w['value'] and not kon:
                if on>=len(r['keyons']) or (w['half_cycle'],w['value'])!=(r['keyons'][on]['half_cycle'],r['keyons'][on]['mask']):raise ValueError('final peer KON differs from accepted write')
                on+=1
            kon=w['value']
        else:
            asserted=(w['value']&~kof)&12;kof=w['value']
            if asserted:
                if off>=len(r['keyoffs']) or (w['half_cycle'],asserted,kof)!=(r['keyoffs'][off]['half_cycle'],r['keyoffs'][off]['mask'],r['keyoffs'][off]['held_mask']):raise ValueError('final peer KOF differs from accepted write')
                off+=1
    if (on,off)!=(len(r['keyons']),len(r['keyoffs'])) or kon or kof:raise ValueError('final peer key ledger incomplete')
    controls=control_writes(r)
    if [(w['address'],w['value']) for w in controls]!=[(92,255),(92,0),(76,0)] or keys[-3:]!=controls:
        raise ValueError('final peer stop pulse differs')
    if any(not 0<=w['half_cycle']-controls[0]['half_cycle']<=8192 for w in controls):raise ValueError('final peer stop writes outside timing bound')
    # Clearing KOF must leave the DSP in its release state. The probe also
    # checks both ENVX registers settle to zero and restores during the pulse.
    if any(not integer(r.get(k),0,0) for k in ('final_observation_half_cycles','final_env_start','final_env_end')):
        raise ValueError('final peer incorrectly reported a held terminal note')
    if any(not integer(r['final_tail_pcm'].get(k),0,0) for k in ('frames','nonzero_frames','peak','quiet_tail_frames')):
        raise ValueError('final peer incorrectly used pending-note audio mode')


def align(r,data):
    validate(r);peer_align(peer_report(r),data)


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,refs):
    agree(refs)
    if not isinstance(candidates,dict) or set(candidates)!={f'{c}-{a}' for c in CASES for a in (63,127)}:
        raise ValueError('require complete final peer native matrix')
    result=[]
    for ref in refs:
        c,a,m=(ref[k] for k in ('case','articulation','model'));r=candidates[f'{c}-{a}'];align(r,bank(a,c))
        if ref.get('fixture_sha256')!=hashlib.sha256(build(a,c)).hexdigest():raise ValueError('final peer owned cartridge hash differs')
        targets=expected(c)
        if len(r['keyons'])!=len(targets) or r['pattern_ticks']!=([0,32] if c.startswith('inactive') else [0]) or r['pattern_masks']!=([12,1<<int(c[-1])] if c.startswith('inactive') else [12]) or r['end_tick']!=(64 if c.startswith('inactive') else 32):
            raise ValueError('final peer pattern/onset geometry differs')
        for e,w in zip(r['keyons'],targets):
            if e['mask']!=w['mask'] or any(e['pitches'][v['voice']-2]!=v['pitch'] or e['volumes'][v['voice']-2]!=v['volumes'] for v in w['voices']):
                raise ValueError('final peer live voice differs')
        if c.startswith('inactive') and a==127 and not r['frozen_peer_checks'][3-int(c[-1])]:raise ValueError('permanently inactive peer was not observed frozen')
        gates,held=observation(r);controls=control_writes(r);stop=controls[0]['half_cycle']
        if held or len(gates)!=len(ref['note_gates']):raise ValueError('final peer remained held or release count differs')
        if r['keyoffs'][-1]['volumes']!=ref['final_volumes']:raise ValueError('final peer terminal DSP volumes differ')
        for g,o in zip(gates,ref['note_gates']):
            if (g['voice'],g['onset_index'])!=(o['voice'],o['onset_index']) or g['cause']!=(0 if o['release_kind']=='final-stop' else 1):
                raise ValueError('final peer release source/identity differs')
        pending=[g['voice'] for g in gates if g['cause']==0]
        if pending!=ref['voices_pending_at_stop'] or any(e['half_cycle']!=stop or e['held_mask']!=255 for e in r['keyoffs'] if e['cause']==0):
            raise ValueError('final peer release not bound to stop pulse')
        gd=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,ref['note_gates'])]
        intervals=[(y['half_cycle']-x['half_cycle'])/2 for x,y in zip(r['keyons'],r['keyons'][1:])]
        od=[abs(x-y) for x,y in zip(intervals,ref['onset_intervals_spc_cycles'])]
        sd=abs((stop-r['keyons'][-1]['half_cycle'])/2-ref['final_stop_interval_spc_cycles'])
        cd=[abs((w['half_cycle']-stop)/2-o) for w,o in zip(controls,ref['final_control_offsets_spc_cycles'])]
        if any(v>4096 for v in gd+od+cd+[sd]):raise ValueError('final peer exceeds retained timing allowance')
        result.append({'case':c,'articulation':a,'model':m,'absolute_gate_difference_spc_cycles':gd,
                       'absolute_onset_difference_spc_cycles':od,'absolute_stop_difference_spc_cycles':sd,
                       'absolute_control_difference_spc_cycles':cd})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            if args.reference.stat().st_size>1024*1024:raise ValueError('final peer reference exceeds JSON bound')
            loaded=json.loads(args.reference.read_text())
            if loaded.get('schema')!=REF_SCHEMA or loaded.get('qualification') is not False or loaded.get('playback') is not False:raise ValueError('invalid final peer observation artifact')
            refs=loaded['reference']
        else:
            if not args.trace or not args.firmware_dir:p.error('provide --reference or --trace and --firmware-dir')
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,c) for c in CASES for a in (63,127) for m in ('sgb','sgb2')]
        candidates={f'{c}-{a}':native(args.probe.resolve(),bank(a,c)) for c in CASES for a in (63,127)}
        result={'schema':'gbb-score-final-peer-playback-reference-v1','qualification':False,'playback':False,
                'program_sha256':hashlib.sha256(program_build()).hexdigest(),'native':candidates,'reference':refs,
                'comparisons':compare(candidates,refs),'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
