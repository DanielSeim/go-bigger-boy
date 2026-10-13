#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Delay an authored E7 without moving its operand; retain interrupted playback."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import tempfile
from build_sgb_vendor_tick_selection_fixture import build
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_tempo_publication_reference import summarize as publication_summary

# Fixed before the full reference sweep; no selection by observed wait/error.
GAPS={4:{'sgb':(8,9,10),'sgb2':(4,5,6)},
      8:{'sgb':(10,11,12),'sgb2':(6,7,8)}}
SPINS=(0,512,1024,2048)


def cases():
    return [(m,l,d,g,s) for d,models in GAPS.items() for m,gaps in models.items()
            for l in (32,128) for g in gaps for s in SPINS]


def summarize(data,live,duration):
    if type(duration) is not int or duration not in GAPS:
        raise ValueError('invalid delayed prefix duration')
    result=publication_summary(data,live,prefix_duration=duration)
    first=next(e['half'] for e in data['events'] if e['register']==0x4C)
    writes=result['handoff']['music_writes']
    established=not result['startup_overlap'] and bool(writes) and all(first<w['write_half'] for w in writes)
    crossing=established and result['publication_order']=='all_writes_before_operand' and result['operand_before_directory'] is True
    # Multiple candidates are retained; never pick the one that fits an outcome.
    unique=crossing and len(writes)==1
    first_crossing=established and len(result['operand_reads'])==1 and result['operand_before_directory'] is True and writes[0]['operand_after_write_ms']>0
    prediction='not_distinguished'
    if first_crossing:
        starting=result['starting_within_2ms'];later=result['live_within_2ms']
        prediction=('control_agrees' if live==128 and starting and later else
                    'starting_tempo_only' if starting and not later else
                    'later_tempo_only' if later and not starting else 'both' if starting and later else 'neither')
    return dict(**result,established_before_publication=established,
                publication_crossing=crossing,unique_publication_crossing=unique,
                timing_evidence=prediction if unique else 'not_distinguished',
                first_publication_before_operand=first_crossing,first_publication_evidence=prediction)


def compare(runs):
    coordinates=set(cases());by_case={}
    for row in runs:
        if (not isinstance(row,dict) or any(type(row[k]) is not int for k in ('live','duration','gap','spin')) or
                type(row['wait_ms']) not in (int,float) or not 0<=row['wait_ms']<=100 or
                any(type(row[k]) is not bool for k in ('startup_overlap','starting_within_2ms','live_within_2ms',
                    'established_before_publication','publication_crossing','unique_publication_crossing','first_publication_before_operand'))):
            raise ValueError('invalid delayed tempo corpus fields')
        key=tuple(row[k] for k in ('model','live','duration','gap','spin'))
        if key not in coordinates or key in by_case:raise ValueError('invalid delayed tempo coordinate')
        by_case[key]=row
    if set(by_case)!=coordinates:raise ValueError('incomplete delayed tempo corpus')
    pairs=[]
    for model,live,duration,gap,spin in cases():
        if live!=32:continue
        changed=by_case[model,32,duration,gap,spin];control=by_case[model,128,duration,gap,spin]
        pairs.append(dict(model=model,duration=duration,gap=gap,spin=spin,
            changed_order=changed['publication_order'],control_order=control['publication_order'],
            changed_overlap=changed['startup_overlap'],control_overlap=control['startup_overlap'],
            changed_unique_crossing=changed['unique_publication_crossing'],control_unique_crossing=control['unique_publication_crossing'],
            changed_first_crossing=changed['first_publication_before_operand'],control_first_crossing=control['first_publication_before_operand'],
            changed_first_evidence=changed['first_publication_evidence'],control_first_evidence=control['first_publication_evidence'],
            changed_wait_ms=changed['wait_ms'],control_wait_ms=control['wait_ms'],
            difference_ms=changed['wait_ms']-control['wait_ms'],changed_evidence=changed['timing_evidence'],
            control_evidence=control['timing_evidence']))
    return dict(parameters_fitted=False,pairs=pairs,
                distinguishing_pairs=sum(p['changed_unique_crossing'] and p['control_unique_crossing'] for p in pairs),
                first_publication_pairs=sum(p['changed_first_crossing'] and p['control_first_crossing'] for p in pairs),
                later_tempo_after_first_publication_pairs=sum(p['changed_first_crossing'] and p['control_first_crossing']
                    and p['changed_first_evidence']=='later_tempo_only' and p['control_first_evidence']=='control_agrees' for p in pairs))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-delayed-tempo-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            probe=root/args.probe.name;shutil.copy2(args.probe.resolve(),probe)
            programs={}
            for name in ('sgb1.program.rom','sgb2.program.rom'):
                programs[name]=root/name;shutil.copy2(args.firmware_dir/name,programs[name])
            def run(case):
                model,live,duration,gap,spin=case
                image=build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,
                            selection_spin=spin,prefix_duration=duration)
                game=root/f'{model}-{live}-{duration}-{gap}-{spin}.gb';game.write_bytes(image)
                program=programs['sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom']
                data=capture(probe.resolve(),program.resolve(),game,model,300000000,inputs,tempo_publication=True)
                return dict(model=model,live=live,duration=duration,gap=gap,spin=spin,
                    fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data,live,duration))
            with ThreadPoolExecutor(max_workers=4) as pool:runs=list(pool.map(run,cases()))
            report=dict(schema='gbb-sgb-vendor-delayed-tempo-v1',qualification=False,parameters_fitted=False,
                probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                program_sha256={n:hashlib.sha256(path.read_bytes()).hexdigest() for n,path in programs.items()},
                runs=runs,comparison=compare(runs))
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded delayed-tempo experiment failed; child diagnostics suppressed\n')


if __name__=='__main__':main()
