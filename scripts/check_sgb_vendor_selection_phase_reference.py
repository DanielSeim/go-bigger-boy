#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Separate authored note duration, first request and arrival phase opaquely."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_selection_wait_fixture import build
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_traversal_reference import summarize


def observation(data):
    # Retain all existing note ownership, completed-state and traversal guards.
    selections=summarize(data,'base','completed')
    notes=[e for e in data['events'] if e['register']==0x4C]
    request=data['requests'][1]
    release=next(e['half'] for e in data['events'] if e['register']==0x5C and
                 e['value']==255 and notes[0]['half']<e['half']<request)
    directory=max(r['half'] for r in data['selection_directory_reads']
                  if r['request']==2 and r['address'] in (0x2B02,0x2B03))
    first_directory=max(r['half'] for r in data['selection_directory_reads']
                        if r['request']==1 and r['address'] in (0x2B00,0x2B01))
    first_root=min(r['half'] for r in data['selection_traversal_reads']
                   if r['request']==1 and r['address']==0x2B10 and r['half']>=first_directory)
    root=min(r['half'] for r in data['selection_traversal_reads']
             if r['request']==2 and r['address']==0x2B14 and r['half']>=directory)
    return dict(first_request_half=data['requests'][0],request_half=request,
                first_root_half=first_root,directory_half=directory,root_half=root,
                request_gap_ms=(request-data['requests'][0])/2048,
                prior_note_to_request_ms=(request-notes[0]['half'])/2048,
                prior_release_to_request_ms=(request-release)/2048,
                root_wait_ms=selections[1]['traversal_stages']['root']['after_previous_stage_ms'],
                control_root_wait_ms=selections[2]['traversal_stages']['root']['after_previous_stage_ms'])


def constant_bound(values):
    if not values or any(type(v) not in (int,float) or not 0<=v<=100 for v in values):
        raise ValueError('invalid bounded root times')
    low=max(values)-2;high=min(values)+2
    return dict(min_ms=min(values),max_ms=max(values),spread_ms=max(values)-min(values),
                constant_within_2ms_possible=low<=high,
                admissible_constant_ms=[low,high] if low<=high else None)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--duration',type=int,choices=(24,48,96),action='append')
    parser.add_argument('--request-offset',type=int,choices=range(5),action='append')
    parser.add_argument('--first-request-offset',type=int,choices=range(5),action='append')
    parser.add_argument('--selection-spin',type=int,choices=(0,512,1024,1536,2048),action='append')
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-selection-phase-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,duration,offset,first,spin)
                   for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for duration in dict.fromkeys(args.duration or (24,48,96))
                   for offset in dict.fromkeys(args.request_offset or range(5))
                   for first in dict.fromkeys(args.first_request_offset or (0,))
                   for spin in dict.fromkeys(args.selection_spin or (0,))]
            def run(case):
                model,name,duration,offset,first,spin=case
                image=build(112,'completed',request_offset=offset,first_duration=duration,
                            first_request_offset=first,selection_spin=spin)
                game=root/f'{model}-{duration}-{offset}-{first}-{spin}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             260000000,inputs,selection_traversal=True)
                return dict(model=model,first_duration=duration,request_offset=offset,
                            first_request_offset=first,selection_spin=spin,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),**observation(data))
            with ThreadPoolExecutor(max_workers=4) as pool:
                runs=list(pool.map(run,cases))
            groups=[dict(model=model,first_duration=duration,
                         **constant_bound([r['root_wait_ms'] for r in runs
                                           if r['model']==model and r['first_duration']==duration]))
                    for model in ('sgb','sgb2') for duration in dict.fromkeys(args.duration or (24,48,96))]
            report=dict(schema='gbb-sgb-vendor-selection-phase-v2',qualification=False,tempo=112,
                        program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                        for name in ('sgb1.program.rom','sgb2.program.rom')},
                        constant_bound=constant_bound([r['root_wait_ms'] for r in runs]),groups=groups,runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded selection-phase experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
