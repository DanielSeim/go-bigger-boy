#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded hardware timer consumption across preceding tempos and states."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_selection_wait_fixture import build,TEMPOS
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_selection_phase_reference import observation,constant_bound

TEMPO_POINTS=(*TEMPOS,112)


def decomposition(measured):
    access=measured['timers'][1]['wait'][0]
    pulses=access['nonzero_after_stage_ms']
    gaps=[b-a for a,b in zip(pulses,pulses[1:])]
    return dict(timer0_nonzero_reads=len(pulses),timer0_zero_reads=access['zero_count'],
                first_nonzero_ms=pulses[0] if pulses else None,
                last_nonzero_ms=pulses[-1] if pulses else None,
                min_nonzero_gap_ms=min(gaps) if gaps else None,
                max_nonzero_gap_ms=max(gaps) if gaps else None,
                after_last_nonzero_ms=measured['root_wait_ms']-pulses[-1] if pulses else None)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--tempo',type=int,choices=TEMPO_POINTS,action='append')
    parser.add_argument('--state',choices=('active','completed'),action='append')
    parser.add_argument('--request-offset',type=int,choices=range(5),default=0)
    parser.add_argument('--first-request-offset',type=int,choices=range(5),default=0)
    parser.add_argument('--selection-spin',type=int,choices=(0,512,1024,1536,2048),default=0)
    args=parser.parse_args()
    try:
        tempos=tuple(dict.fromkeys(args.tempo or TEMPO_POINTS))
        states=tuple(dict.fromkeys(args.state or ('active','completed')))
        with tempfile.TemporaryDirectory(prefix='gbb-timer-tempo-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,tempo,state)
                   for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for tempo in tempos for state in states]
            def run(case):
                model,name,tempo,state=case
                image=build(tempo,state,request_offset=args.request_offset,
                            first_request_offset=args.first_request_offset,selection_spin=args.selection_spin,
                            first_duration=96,completed_frames=124)
                game=root/f'{model}-{tempo}-{state}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             300000000,inputs,selection_timer_results=True)
                if data.get('selection_timer_results') is not True:
                    raise ValueError('missing requested timer results')
                measured=observation(data,state)
                return dict(model=model,tempo=tempo,state=state,
                            first_duration=96,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),
                            decomposition=decomposition(measured),**measured)
            with ThreadPoolExecutor(max_workers=4) as pool:
                runs=list(pool.map(run,cases))
            groups=[dict(tempo=tempo,state=state,
                         **constant_bound([r['root_wait_ms'] for r in runs if r['tempo']==tempo and r['state']==state]))
                    for tempo in tempos for state in states]
            report=dict(schema='gbb-sgb-vendor-timer-tempo-v1',qualification=False,
                        completed_frames=124,first_duration=96,
                        request_offset=args.request_offset,first_request_offset=args.first_request_offset,
                        selection_spin=args.selection_spin,
                        program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                        for name in ('sgb1.program.rom','sgb2.program.rom')},groups=groups,runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded timer-tempo experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
