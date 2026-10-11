#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure request-frame phase against directory/root milestones, without a pacing law."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_request_phase_fixture import build, request_frames, SCHEDULES, PROFILES, STATES
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_traversal_reference import summarize as traversal_summary


def summarize(data,schedule,profile,state):
    frames=request_frames(schedule,state)
    if profile not in PROFILES: raise ValueError('invalid phase STOP profile')
    selections=traversal_summary(data,profile,state)
    requests=[i for i,m in enumerate(data['music']) if m]
    gaps=[(data['requests'][b]-data['requests'][a])/2048 for a,b in zip(requests,requests[1:])]
    # Keep measured frame gaps visible; physical delivery is not a fractional
    # GB frame oracle. Do not substitute authored frame counts for timestamps.
    return dict(request_frames=list(frames),request_gaps_ms=gaps,selections=selections)


def compare(baseline,measured):
    if (baseline['request_frames'] not in ([16,16,16],[16,100,100]) or
            len(baseline['selections'])!=3 or len(measured['selections'])!=3 or
            [s['music'] for s in baseline['selections']] != [s['music'] for s in measured['selections']]):
        raise ValueError('invalid phase comparison baseline')
    return [dict(music=b['music'],selection_delta_ms=b['selection_ms']-a['selection_ms'],
        directory_delta_ms=b['last_directory_read_ms']-a['last_directory_read_ms'],
        directory_to_root_delta_ms=b['traversal_stages']['root']['after_previous_stage_ms']-
                                  a['traversal_stages']['root']['after_previous_stage_ms'])
        for a,b in zip(baseline['selections'],measured['selections'])]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--model',choices=('sgb','sgb2'),action='append')
    parser.add_argument('--schedule',choices=SCHEDULES,action='append')
    args=parser.parse_args()
    try:
        schedules=tuple(dict.fromkeys(('baseline',*(args.schedule or SCHEDULES))))
        fixtures={(schedule,p,s):build(schedule,p,s) for schedule in schedules for p in PROFILES for s in STATES}
        with tempfile.TemporaryDirectory(prefix='gbb-request-phase-') as temporary:
            root=Path(temporary);owned=root/'owned.rom';owned.write_bytes(firmware())
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            runs=[]
            for model in args.model or ('sgb','sgb2'):
                original=args.firmware_dir/('sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom')
                cases={};baselines={}
                for (schedule,profile,state),image in fixtures.items():
                    game=root/'game.gb';game.write_bytes(image)
                    measured={kind:summarize(capture(args.probe.resolve(),program.resolve(),game,model,
                        260000000,inputs,selection_traversal=True),schedule,profile,state)
                        for kind,program in (('original',original),('replacement',owned))}
                    if schedule=='baseline': baselines[profile,state]=measured
                    cases['-'.join((schedule,profile,state))]=dict(fixture_sha256=hashlib.sha256(image).hexdigest(),
                        **measured,delta_from_baseline={kind:compare(baselines[profile,state][kind],measured[kind])
                                                     for kind in measured})
                runs.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private request-phase experiment failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-request-phase-v1',qualification=False,
        firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=runs)))


if __name__=='__main__': main()
