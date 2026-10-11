#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure authored prior-tempo dependence of selected score-root consumption."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_prior_tempo_fixture import build, request_frames, TEMPOS, SCHEDULES, PROFILES, STATES
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_traversal_reference import summarize as traversal_summary


def summarize(data,tempo,schedule,profile,state):
    if type(tempo) is not int or tempo not in TEMPOS or profile not in PROFILES:
        raise ValueError('invalid prior-tempo control')
    frames=request_frames(schedule,state)
    selections=traversal_summary(data,profile,state)
    requests=[i for i,m in enumerate(data['music']) if m]
    return dict(track_tempos=[tempo,45,45],authored_preceding_tempos=[None,tempo,45],
        request_frames=list(frames),request_gaps_ms=[(data['requests'][b]-data['requests'][a])/2048
            for a,b in zip(requests,requests[1:])],selections=selections)


def compare(baseline,measured):
    if (baseline['track_tempos']!=[45,45,45] or baseline['request_frames']!=measured['request_frames'] or
            len(baseline['selections'])!=3 or len(measured['selections'])!=3 or
            [s['music'] for s in baseline['selections']] != [s['music'] for s in measured['selections']]):
        raise ValueError('invalid matching tempo-45 baseline')
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
    parser.add_argument('--tempo',type=int,choices=TEMPOS,action='append')
    parser.add_argument('--schedule',choices=SCHEDULES,action='append')
    args=parser.parse_args()
    try:
        tempos=tuple(dict.fromkeys((45,*(args.tempo or TEMPOS))))
        schedules=tuple(dict.fromkeys(args.schedule or SCHEDULES))
        fixtures={(t,s,p,state):build(t,s,p,state) for t in tempos for s in schedules
                  for p in PROFILES for state in STATES}
        with tempfile.TemporaryDirectory(prefix='gbb-prior-tempo-') as temporary:
            root=Path(temporary);owned=root/'owned.rom';owned.write_bytes(firmware())
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            runs=[]
            for model in args.model or ('sgb','sgb2'):
                original=args.firmware_dir/('sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom')
                cases={};baselines={}
                for (tempo,schedule,profile,state),image in fixtures.items():
                    game=root/'game.gb';game.write_bytes(image)
                    measured={kind:summarize(capture(args.probe.resolve(),program.resolve(),game,model,
                        260000000,inputs,selection_traversal=True),tempo,schedule,profile,state)
                        for kind,program in (('original',original),('replacement',owned))}
                    if tempo==45: baselines[schedule,profile,state]=measured
                    cases['-'.join((str(tempo),schedule,profile,state))]=dict(
                        fixture_sha256=hashlib.sha256(image).hexdigest(),**measured,
                        delta_from_tempo45={kind:compare(baselines[schedule,profile,state][kind],measured[kind])
                                            for kind in measured})
                runs.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private prior-tempo experiment failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-prior-tempo-v1',qualification=False,
        firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=runs)))


if __name__=='__main__': main()
