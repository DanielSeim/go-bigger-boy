#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolate uploaded-score selection workload and request phase; metadata only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_selection_fixture import build, PROFILES, STATES
from check_sgb_vendor_music_reference import capture


def summarize(data, state):
    requests = [i+1 for i,music in enumerate(data['music']) if music==1]
    notes = [event for event in data['events'] if event['register']==0x4C]
    if (state not in STATES or len(requests)!=2 or len(notes)!=2 or
            any(music not in (0,1) for music in data['music']) or
            [note['request'] for note in notes]!=requests or
            any([note[k] for k in ('pitch','srcn','adsr1','adsr2','gain')] != [1437,2,255,224,184] for note in notes)):
        raise ValueError('constant-note selection contract changed')
    completed = any(event['register']==0x5C and event['value']==255 and
                    notes[0]['half']<event['half']<data['requests'][requests[1]-1] for event in data['events'])
    if completed != (state=='completed'): raise ValueError('preceding selection state changed')
    if data.get('final') != dict(flg=0,kof=0,echo_left=0,echo_right=0):
        raise ValueError('final selection did not release')
    windows = data.get('port_windows')
    if not isinstance(windows,list) or len(windows)!=2:
        raise ValueError('missing bounded port windows')
    report = []
    for note,window,request in zip(notes,windows,requests):
        start = data['requests'][request-1]
        if (not isinstance(window,dict) or any(type(window.get(k)) is not int for k in ('request','first','last','keyon')) or
                window['request']!=request or window['keyon']!=note['half'] or
                not start<=window['first']<=window['last']<window['keyon'] or
                not isinstance(window.get('writes'),list) or len(window['writes'])!=4 or
                any(type(n) is not int or not 0<=n<=16384 for n in window['writes']) or not sum(window['writes'])):
            raise ValueError('invalid instruction-boundary port window')
        report.append(dict(selection_ms=(note['half']-start)/2048,
                           first_port_ms=(window['first']-start)/2048,
                           last_port_ms=(window['last']-start)/2048,
                           keyon_after_last_port_ms=(note['half']-window['last'])/2048,
                           writes=window['writes']))
    return dict(request_gap_ms=(data['requests'][requests[1]-1]-data['requests'][requests[0]-1])/2048,
                selections=report,first_completed=completed)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--model',choices=('sgb','sgb2'),action='append')
    parser.add_argument('--profile',choices=PROFILES,action='append')
    parser.add_argument('--state',choices=STATES,action='append')
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-selection-isolation-') as directory:
            base=Path(directory); owned=base/'owned.rom'; owned.write_bytes(firmware())
            inputs=base/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            # Validate every fixture before starting any private execution.
            fixtures={(profile,state):build(profile,state) for profile in args.profile or PROFILES for state in args.state or STATES}
            reports=[]
            for model in args.model or ('sgb','sgb2'):
                original=args.firmware_dir/('sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom')
                result=dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases={})
                for (profile,state),image in fixtures.items():
                    game=base/'game.gb'; game.write_bytes(image)
                    measured={kind:summarize(capture(args.probe.resolve(),program.resolve(),game,model,240000000,inputs),state)
                              for kind,program in (('original',original),('replacement',owned))}
                    result['cases'][profile+'-'+state]=dict(fixture_sha256=hashlib.sha256(image).hexdigest(),**measured)
                reports.append(result)
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private selection experiment failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-selection-v1',qualification=False,
                         firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=reports)))


if __name__=='__main__': main()
