#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe authored repeated-note music ticks through public DSP writes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_tick_fixture import build,NOTES,DURATION,TEMPO_POINTS
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_timer_tempo_reference import continuity

def summarize(data):
    if data['music']!=[1] or len(data['requests'])!=1 or data.get('selection_timer_results') is not True:
        raise ValueError('tick request or observer contract changed')
    notes=[e for e in data['events'] if e['register']==0x4C]
    if (len(notes)!=len(NOTES) or [e['pitch'] for e in notes]!=[1437,1357]*4 or
            any(e['request']!=1 or e['srcn']!=2 or e['adsr1']!=255 or e['adsr2']!=224 or e['gain']!=184 for e in notes)):
        raise ValueError('tick note ownership or uploaded binding changed')
    completion=next((e['half'] for e in data['events'] if e['register']==0x5C and
                     e['value']==255 and e['half']>notes[-1]['half']),None)
    completion_clear=next((e['half'] for e in data['events'] if e['register']==0x5C and
                           e['value']==0 and completion is not None and e['half']>completion),None)
    if completion is None or completion_clear is None:
        raise ValueError('missing tick completion or completion clear')
    gates=[];clears=[]
    for i,note in enumerate(notes):
        end=notes[i+1]['half'] if i+1<len(notes) else completion
        gate=next((e['half'] for e in data['events'] if e['register']==0x5C and
                   e['value']==4 and note['half']<e['half']<end),None)
        clear=next((e['half'] for e in data['events'] if e['register']==0x5C and
                    e['value']==0 and gate is not None and gate<e['half']<end),None)
        if gate is None or clear is None and i+1<len(notes):
            raise ValueError('missing tick gate or gate clear')
        gates.append(gate);clears.append(clear)
    if any(e['register']==0x5C and e['value']==255 and notes[0]['half']<e['half']<completion for e in data['events']):
        raise ValueError('tick song completed before its final note')
    # Continuity is per request, whereas the score emits multiple notes.
    # Retain only the first KON for its ownership validator; all KOF markers stay.
    collapsed={**data,'events':[e for e in data['events'] if e['register']!=0x4C or e is notes[0]]}
    windows=continuity(collapsed)['windows']
    if windows[0]['completion_half']!=completion or not windows[0]['playback'][0]['nonzero']['count']:
        raise ValueError('tick completion anchor disagrees')
    start=notes[0]['half']
    return dict(first_keyon_half=start,onsets_half=[e['half']-start for e in notes],
                gates_half=[t-start for t in gates],gate_clear_half=[b-a if b is not None else None for a,b in zip(gates,clears)],
                completion_half=completion-start,completion_clear_half=completion_clear-completion,
                timers=windows[0])


def expected(tempo,phase,*,duration=DURATION):
    if type(tempo) is not int or tempo not in TEMPO_POINTS or type(phase) is not int or not 0<=phase<=255:
        raise ValueError('invalid tick model parameters')
    if type(duration) is not int or not 1<=duration<=96:
        raise ValueError('invalid tick duration')
    # A continuous modulo-256 accumulator; no per-note fractional reset.
    return [((duration*i*256-phase+tempo-1)//tempo)*4096 for i in range(1,len(NOTES)+1)]


def compare(runs):
    result=[]
    for model in ('sgb','sgb2'):
        for tempo in sorted({r['tempo'] for r in runs}):
            baseline=next(r for r in runs if r['model']==model and r['tempo']==tempo and not r['holdout'])
            holdout=next(r for r in runs if r['model']==model and r['tempo']==tempo and r['holdout'])
            def errors(run,phase):
                return [abs(a-b) for a,b in zip(expected(tempo,phase)[:-1],run['onsets_half'][1:])]
            def steady_errors(run,phase):
                return [abs(a-(b-run['onsets_half'][1])) for a,b in
                        zip(expected(tempo,phase)[:len(NOTES)-2],run['onsets_half'][2:])]
            phase=min(range(256),key=lambda p:(max(errors(baseline,p)),sum(errors(baseline,p)),p))
            steady_phase=min(range(256),key=lambda p:(max(steady_errors(baseline,p)),sum(steady_errors(baseline,p)),p))
            prediction=expected(tempo,phase)
            cases=[]
            for run in (baseline,holdout):
                differences=errors(run,phase)
                independent=min(range(256),key=lambda p:(max(errors(run,p)),sum(errors(run,p)),p))
                steady=steady_errors(run,steady_phase)
                cases.append(dict(holdout=run['holdout'],max_onset_error_ms=max(differences)/2048,
                    onsets_within_2ms=all(e<=4096 for e in differences),
                    completion_error_ms=abs(prediction[-1]-run['completion_half'])/2048,
                    steady_max_onset_error_ms=max(steady)/2048,
                    steady_onsets_within_2ms=all(e<=4096 for e in steady),
                    independent_phase_fit=dict(diagnostic_only=True,phase=independent,
                        max_onset_error_ms=max(errors(run,independent))/2048)))
            result.append(dict(model=model,tempo=tempo,baseline_phase=phase,
                               baseline_steady_phase=steady_phase,cases=cases))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--tempo',type=int,choices=TEMPO_POINTS,action='append')
    args=parser.parse_args()
    try:
        tempos=tuple(dict.fromkeys(args.tempo or TEMPO_POINTS))
        with tempfile.TemporaryDirectory(prefix='gbb-tick-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,tempo,holdout) for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for tempo in tempos for holdout in (False,True)]
            def run(case):
                model,name,tempo,holdout=case;image=build(tempo,holdout=holdout)
                game=root/f'{model}-{tempo}-{int(holdout)}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             200000000,inputs,timer_continuity=True)
                return dict(model=model,tempo=tempo,holdout=holdout,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            report=dict(schema='gbb-sgb-vendor-ticks-v1',qualification=False,duration=DURATION,notes=len(NOTES),
                heldout_scope='phase_parameters_only',
                program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                for name in ('sgb1.program.rom','sgb2.program.rom')},
                comparisons=compare(runs),runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded music-tick experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
