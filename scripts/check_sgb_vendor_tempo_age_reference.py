#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bound selection's live-tempo adoption with late authored tempo commands."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_tick_selection_reference import summarize as phase_summary
from check_sgb_vendor_tick_reference import expected
from check_sgb_vendor_timer_writes_reference import configuration
from check_sgb_vendor_tempo_change_reference import TEMPOS

GAPS=(16,20)


def operand_reads(data):
    reads=data.get('tempo_operand_reads')
    if not isinstance(reads,list) or not 1<=len(reads)<=64:
        raise ValueError('missing or excessive operand observations')
    first={}
    for event in data['events']:
        if event['register']==0x4C: first.setdefault(event['request'],event['half'])
    end=data['timer_configuration']['end_half'];previous=(-1,0)
    for read in reads:
        if (not isinstance(read,dict) or set(read)!={'half','request','address'} or
                any(type(v) is not int for v in read.values()) or read['address']!=0x2B70 or
                not 1<=read['request']<=len(data['requests'])):
            raise ValueError('invalid operand observation')
        request=read['request'];half=read['half']
        if not first[request]<=half<=(data['requests'][request] if request<len(data['requests']) else end) or (half,request)<previous:
            raise ValueError('invalid operand ownership/order')
        previous=(half,request)
    notes=[e for e in data['events'] if e['register']==0x4C and e['request']==1]
    consumed=[r['half'] for r in reads if r['request']==1]
    if len(notes)!=5 or not consumed or any(not notes[2]['half']<t<notes[3]['half'] for t in consumed):
        raise ValueError('operand was not consumed between authored notes')
    return dict(reads=reads,last_operand_half=consumed[-1],
        operand_to_new_note_ms=(notes[3]['half']-consumed[-1])/2048,
        operand_age_ms=(data['requests'][1]-consumed[-1])/2048)


def summarize(data,start,live):
    if any(type(t) is not int or t not in TEMPOS for t in (start,live)):
        raise ValueError('invalid adoption tempo controls')
    measured=phase_summary(data,live,'active',witness_note=3)
    times=measured['prefix_onsets_half']
    bounds=[min(expected(start,p)[0] for p in range(256)),max(expected(start,p)[0] for p in range(256))]
    def errors(p): return [abs(a-b) for a,b in zip(expected(start,p)[:2],(t-times[1] for t in times[2:4]))]
    phase=min(range(256),key=lambda p:(max(errors(p)),sum(errors(p)),p))
    if not bounds[0]-8192<=times[1]<=bounds[1]+8192 or max(errors(phase))>4096 or measured['prefix_max_error_ms']>2:
        raise ValueError('late tempo witnesses changed')
    # The fourth KON follows the authored E7. Its age is a conservative lower
    # bound on command age, not an observation of original instruction timing.
    return dict(configuration=configuration(data),operand=operand_reads(data),starting_phase=phase,
        starting_prefix_error_ms=max(errors(phase))/2048,
        new_tempo_note_age_ms=(measured['request_half']-measured['keyon'])/2048,**measured)


def compare(runs):
    contrasts=[]
    for model in ('sgb','sgb2'):
        for gap in GAPS:
            for holdout in (False,True):
                group=[r for r in runs if (r['model'],r['gap'],r['holdout'])==(model,gap,holdout)]
                if len(group)!=4 or {(r['start'],r['live']) for r in group}!={(a,b) for a in TEMPOS for b in TEMPOS}:
                    raise ValueError('missing or duplicated adoption controls')
                controls={r['start']:r['observed']/2048 for r in group if r['start']==r['live']}
                for row in group:
                    if row['start']==row['live']: continue
                    observed=row['observed']/2048
                    start_error=abs(observed-controls[row['start']]);live_error=abs(observed-controls[row['live']])
                    contrasts.append(dict(model=model,gap=gap,holdout=holdout,start=row['start'],live=row['live'],
                        new_tempo_note_age_ms=row['new_tempo_note_age_ms'],observed_ms=observed,
                        control_separation_ms=abs(controls[32]-controls[128]),
                        starting_error_ms=start_error,live_error_ms=live_error,
                        starting_within_2ms=start_error<=2,live_within_2ms=live_error<=2))
    if len(runs)!=32: raise ValueError('invalid adoption corpus size')
    # The later request must actually increase observed note age at every
    # matched tempo pair beyond the 2-ms comparison resolution. Do not assume
    # identical startup phase merely because both images request four more frames.
    age_pairs=[]
    for model in ('sgb','sgb2'):
        for start in TEMPOS:
            for live in TEMPOS:
                for holdout in (False,True):
                    ages={r['gap']:r['new_tempo_note_age_ms'] for r in runs if
                          (r['model'],r['start'],r['live'],r['holdout'])==(model,start,live,holdout)}
                    delta=ages[20]-ages[16]
                    if min(ages.values())<=0 or delta<=2:
                        raise ValueError('request age controls did not separate')
                    age_pairs.append(dict(model=model,start=start,live=live,holdout=holdout,age_increase_ms=delta))
    return dict(parameters_fitted=False,tolerance_ms=2,age_pairs=age_pairs,cases=contrasts)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-tempo-age-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,start,live,gap,holdout)
                for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                for start in TEMPOS for live in TEMPOS for gap in GAPS for holdout in (False,True)]
            def run(case):
                model,name,start,live,gap,holdout=case
                image=build(start,'active',live_tempo=live,change_after=3,request_gap=gap,holdout=holdout)
                game=root/f'{model}-{start}-{live}-{gap}-{int(holdout)}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             300000000,inputs,tempo_operands=True)
                return dict(model=model,start=start,live=live,gap=gap,holdout=holdout,
                    fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data,start,live))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            report=dict(schema='gbb-sgb-vendor-tempo-age-v2',qualification=False,
                program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                for name in ('sgb1.program.rom','sgb2.program.rom')},
                comparison=compare(runs),runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded tempo-age experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
