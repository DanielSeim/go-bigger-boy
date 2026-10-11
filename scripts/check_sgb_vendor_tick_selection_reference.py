#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Relate authored music-phase witnesses to bounded reselection timing."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build,TEMPO_POINTS
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_timer_tempo_reference import continuity
from check_sgb_vendor_selection_phase_reference import timer_summary
from check_sgb_vendor_tick_reference import expected
from check_sgb_vendor_accumulator_model import predict


def summarize(data,tempo,state):
    if tempo not in TEMPO_POINTS or type(tempo) is not int or state not in ('active','completed'):
        raise ValueError('invalid phase witness controls')
    if data['music']!=[1,2] or len(data['requests'])!=2 or data.get('selection_timer_results') is not True:
        raise ValueError('phase witness request contract changed')
    notes=[e for e in data['events'] if e['register']==0x4C]
    if ([e['request'] for e in notes]!=[1]*5+[2] or
            [e['pitch'] for e in notes]!=[1437,1357,1437,1357,1281,1357] or
            any(e['srcn']!=2 or e['adsr1']!=255 or e['adsr2']!=224 or e['gain']!=184 for e in notes)):
        raise ValueError('phase witness notes or bindings changed')
    request=data['requests'][1]
    if not notes[4]['half']<request<=notes[5]['half']:
        raise ValueError('long tail was not reached before reselection')
    for note,next_note in zip(notes[:4],notes[1:5]):
        gate=next((e['half'] for e in data['events'] if e['register']==0x5C and e['request']==1 and
                   e['value']==4 and note['half']<e['half']<next_note['half']),None)
        if gate is None or not any(e['register']==0x5C and e['request']==1 and e['value']==0 and
                                   gate<e['half']<next_note['half'] for e in data['events']):
            raise ValueError('phase witness gate or clear missing')
    # Preserve first-KON request ownership while retaining all completion events.
    selected=(notes[0],notes[5])
    collapsed={**data,'events':[e for e in data['events'] if e['register']!=0x4C or any(e is n for n in selected)]}
    windows=continuity(collapsed)['windows']
    completion=windows[0]['completion_half']
    if bool(completion)!=(state=='completed') or completion and completion<=notes[4]['half']:
        raise ValueError('phase witness playback state changed')
    if not windows[1]['completion_half']:
        raise ValueError('selected song did not complete within capture')
    selections=[];anchors=[]
    for i,note in enumerate(selected):
        # Validate the complete selected traversal, keeping only the final live pass.
        directories=[r for r in data['selection_directory_reads'] if r['request']==i+1]
        if any(r['address'] not in (0x2B00+2*i,0x2B01+2*i) or not data['requests'][i]<=r['half']<note['half'] for r in directories):
            raise ValueError('directory ownership changed')
        if {r['address'] for r in directories}!={0x2B00+2*i,0x2B01+2*i}:
            raise ValueError('selected directory missing')
        directory=max(r['half'] for r in directories);previous=directory;root=None
        for low,high,required in ((0x2B10+4*i,0x2B13+4*i,{0x2B10+4*i,0x2B11+4*i}),
                                  (0x2B20+16*i,0x2B2F+16*i,{0x2B24+16*i,0x2B25+16*i}),
                                  (0x2B60+48*i,0x2B6C+48*i,{0x2B60+48*i,0x2B6C+48*i})):
            reads=[r for r in data['selection_traversal_reads'] if r['request']==i+1 and
                   low<=r['address']<=high and previous<=r['half']<note['half']]
            if not required<={r['address'] for r in reads}:
                raise ValueError('selected traversal incomplete')
            if root is None: root=min(r['half'] for r in reads)
            previous=max(r['half'] for r in reads)
        selections.append(dict(traversal_stages=dict(root=dict(after_previous_stage_ms=(root-directory)/2048))))
        anchors.append((directory,root))
    timers=timer_summary(collapsed,selections)
    onset=notes[1]['half']
    witnesses=[e['half']-onset for e in notes[2:5]]
    def error(phase): return [abs(a-b) for a,b in zip(expected(tempo,phase)[:3],witnesses)]
    phase=min(range(256),key=lambda p:(max(error(p)),sum(error(p)),p))
    compatible=[p for p in range(256) if max(error(p))<=2048]
    prior=windows[0]
    terminal=max(prior[s][0]['nonzero']['last'] for s in ('playback','idle'))
    if not terminal:
        raise ValueError('prior timer phase anchor missing')
    return dict(keyon=onset,anchor=terminal,directory=anchors[1][0],observed=anchors[1][1]-anchors[1][0],
                phase=phase,prefix_onsets_half=[e['half']-notes[0]['half'] for e in notes[:5]],
                compatible_prefix_phases=compatible,
                prefix_max_error_ms=max(error(phase))/2048,
                prior_completion_half=completion,request_half=request,
                wait_timer0_nonzero_count=timers[1]['wait'][0]['nonzero_count'],timers=windows)


def compare(runs):
    baseline=[r for r in runs if not r['holdout']]
    if not baseline: raise ValueError('missing baseline phase witnesses')
    def errors(guard,rows): return [abs(predict(r,r['phase'],guard)-r['observed']) for r in rows]
    guard=min(range(17),key=lambda g:(max(errors(g,baseline)),sum(errors(g,baseline)),g))
    # Feasibility deliberately gives each case every phase supported by its
    # prefix. This is an optimistic bound, not a fitted runtime predictor.
    envelope={g:max(min(abs(predict(r,p,g)-r['observed']) for p in r['compatible_prefix_phases'])
                        if r['compatible_prefix_phases'] else 2000000000 for r in baseline)
              for g in range(17)}
    rows=[]
    for run in runs:
        prediction=predict(run,run['phase'],guard);error=abs(prediction-run['observed'])
        alternatives=[predict(run,p,guard) for p in run['compatible_prefix_phases']]
        rows.append(dict(model=run['model'],tempo=run['tempo'],state=run['state'],holdout=run['holdout'],
                         phase=run['phase'],prefix_max_error_ms=run['prefix_max_error_ms'],
                         observed_ms=run['observed']/2048,predicted_ms=prediction/2048,
                         absolute_error_ms=error/2048,within_2ms=error<=4096,
                         compatible_prediction_range_ms=[min(alternatives)/2048,max(alternatives)/2048] if alternatives else None,
                         minimum_compatible_error_ms=min(abs(p-run['observed']) for p in alternatives)/2048 if alternatives else None))
    return dict(guard_opportunities=guard,baseline_feasible_guards=[g for g,e in envelope.items() if e<=4096],
                minimum_baseline_max_error_ms=min(envelope.values())/2048,
                accuracy_passed=all(r['within_2ms'] and r['prefix_max_error_ms']<=2 for r in rows),cases=rows)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--tempo',type=int,choices=TEMPO_POINTS,action='append')
    args=parser.parse_args()
    try:
        tempos=tuple(dict.fromkeys(args.tempo or TEMPO_POINTS))
        with tempfile.TemporaryDirectory(prefix='gbb-tick-selection-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,tempo,state,holdout) for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for tempo in tempos for state in ('active','completed') for holdout in (False,True)]
            def run(case):
                model,name,tempo,state,holdout=case;image=build(tempo,state,holdout=holdout)
                game=root/f'{model}-{tempo}-{state}-{int(holdout)}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             300000000,inputs,timer_continuity=True)
                return dict(model=model,tempo=tempo,state=state,holdout=holdout,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data,tempo,state))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            report=dict(schema='gbb-sgb-vendor-tick-selection-v1',qualification=False,
                program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                for name in ('sgb1.program.rom','sgb2.program.rom')},
                comparison=compare(runs),runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded tick-selection experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
