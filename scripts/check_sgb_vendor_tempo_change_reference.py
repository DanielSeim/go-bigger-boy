#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross starting/live authored tempos without fitting selection outcomes."""
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

TEMPOS=(32,128)


def summarize(data,start_tempo,live_tempo,state):
    if any(type(t) is not int or t not in TEMPOS for t in (start_tempo,live_tempo)):
        raise ValueError('invalid crossed tempos')
    measured=phase_summary(data,live_tempo,state)
    first=measured['prefix_onsets_half'][1]
    bounds=[min(expected(start_tempo,p)[0] for p in range(256)),
            max(expected(start_tempo,p)[0] for p in range(256))]
    # The earlier repeated-note corpus already exposes distinct first-note
    # startup timing (including a 10.159-ms interval at tempo 128). Keep a
    # fixed 4-ms first-interval allowance; later phase witnesses retain 2 ms.
    if not bounds[0]-8192<=first<=bounds[1]+8192 or measured['prefix_max_error_ms']>2:
        raise ValueError('authored starting/live tempo witness changed')
    return dict(configuration=configuration(data),first_interval_half=first,
                starting_interval_bounds_half=bounds,starting_interval_allowance_ms=4,**measured)


def compare(runs):
    # Both unchanged-tempo controls execute E7 too. Each changed case is compared
    # to each possible retained tempo in the same model/state/schedule; no fit.
    rows=[]
    for model in ('sgb','sgb2'):
        for state in ('active','completed'):
            for holdout in (False,True):
                group=[r for r in runs if (r['model'],r['state'],r['holdout'])==(model,state,holdout)]
                if len(group)!=4 or {(r['start_tempo'],r['live_tempo']) for r in group}!={(a,b) for a in TEMPOS for b in TEMPOS}:
                    raise ValueError('missing or duplicated crossed tempo cases')
                controls={r['start_tempo']:r['observed']/2048 for r in group if r['start_tempo']==r['live_tempo']}
                for run in group:
                    if run['start_tempo']==run['live_tempo']: continue
                    observed=run['observed']/2048
                    start_error=abs(observed-controls[run['start_tempo']])
                    live_error=abs(observed-controls[run['live_tempo']])
                    rows.append(dict(model=model,state=state,holdout=holdout,
                        start_tempo=run['start_tempo'],live_tempo=run['live_tempo'],observed_ms=observed,
                        unchanged_control_ms=controls,control_separation_ms=abs(controls[32]-controls[128]),
                        starting_tempo_error_ms=start_error,live_tempo_error_ms=live_error,
                        starting_within_2ms=start_error<=2,live_within_2ms=live_error<=2))
    if len(runs)!=32: raise ValueError('unexpected crossed tempo corpus size')
    return dict(tolerance_ms=2,parameters_fitted=False,cases=rows)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-tempo-change-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,start,live,state,holdout)
                   for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for start in TEMPOS for live in TEMPOS for state in ('active','completed') for holdout in (False,True)]
            def run(case):
                model,name,start,live,state,holdout=case
                image=build(start,state,holdout=holdout,live_tempo=live)
                game=root/f'{model}-{start}-{live}-{state}-{int(holdout)}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             300000000,inputs,timer_writes=True)
                return dict(model=model,start_tempo=start,live_tempo=live,state=state,holdout=holdout,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data,start,live,state))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            report=dict(schema='gbb-sgb-vendor-tempo-change-v1',qualification=False,
                program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                for name in ('sgb1.program.rom','sgb2.program.rom')},
                comparison=compare(runs),runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded tempo-change experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
