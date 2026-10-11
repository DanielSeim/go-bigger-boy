#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare opt-in root pacing with the default DB image and opaque originals."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_selection_wait_fixture import build,TEMPOS
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_traversal_reference import summarize

WAITS={32:32,45:27,64:24,96:19,128:20}


def waits(data,state):
    return [s['traversal_stages']['root']['after_previous_stage_ms'] for s in summarize(data,'base',state)]


def compare(original,baseline,candidate,tempo):
    if type(tempo) is not int or tempo not in WAITS or any(len(v)!=3 for v in (original,baseline,candidate)):
        raise ValueError('invalid selection wait comparison')
    if any(type(v) not in (int,float) or not 0<=v<=100 for a in (original,baseline,candidate) for v in a):
        raise ValueError('invalid bounded wait')
    # First selection remains a distinct, deliberately uncalibrated case.
    if abs(candidate[0]-baseline[0])>0.05:
        raise ValueError('first selection gained an unsupported wait')
    results=[]
    for i,expected in ((1,WAITS[tempo]),(2,WAITS[45])):
        error=abs(candidate[i]-original[i]);before=abs(baseline[i]-original[i])
        if abs(candidate[i]-expected)>0.4 or error>2 or before-error<14:
            raise ValueError('root wait comparison failed its timing/improvement bound')
        results.append(dict(music=i+1,original_ms=original[i],baseline_ms=baseline[i],candidate_ms=candidate[i],
                            absolute_error_ms=error,baseline_absolute_error_ms=before))
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--tempo',type=int,choices=TEMPOS,action='append')
    parser.add_argument('--state',choices=('active','completed'),action='append')
    parser.add_argument('--request-offset',type=int,choices=range(5),default=4)
    args=parser.parse_args()
    try:
        baseline=firmware();candidate=firmware(selection_wait=True);runs=[]
        with tempfile.TemporaryDirectory(prefix='gbb-selection-wait-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            normal=root/'default.rom';normal.write_bytes(baseline);paced=root/'paced.rom';paced.write_bytes(candidate)
            for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom')):
                original=args.firmware_dir/name;cases=[]
                for tempo in tuple(dict.fromkeys(args.tempo or TEMPOS)):
                    for state in tuple(dict.fromkeys(args.state or ('active','completed'))):
                        image=build(tempo,state,request_offset=args.request_offset);game=root/'game.gb';game.write_bytes(image)
                        with ThreadPoolExecutor(max_workers=3) as pool:
                            measured=list(pool.map(lambda p: waits(capture(args.probe.resolve(),p,game,model,
                                260000000,inputs,selection_traversal=True),state),(original,normal,paced)))
                        cases.append(dict(tempo=tempo,state=state,fixture_sha256=hashlib.sha256(image).hexdigest(),
                            first_selection_ms=dict(zip(('original','baseline','candidate'),[a[0] for a in measured])),
                            selections=compare(*measured,tempo)))
                runs.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
        print(json.dumps(dict(schema='gbb-sgb-vendor-selection-wait-v1',qualification=False,opt_in=True,
            request_offset=args.request_offset,baseline_sha256=hashlib.sha256(baseline).hexdigest(),candidate_sha256=hashlib.sha256(candidate).hexdigest(),runs=runs)))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.exit(2,'bounded selection-wait comparison failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
