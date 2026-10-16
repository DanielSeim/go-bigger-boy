#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decompose first selection and independently check calibrated request phases."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_publication_retry_reference import cases, fixture, observe as retry_observe, compare as retry_compare

PHASE_TOLERANCE_MS=2
CALIBRATION_GAPS={'sgb':12,'sgb2':8}


def observe(data,live):
    result=retry_observe(data,live)
    first=result['first_old_keyon_half'];request=data['requests'][0]
    reads=[r for r in data['selection_directory_reads'] if r['request']==1]
    if {r['address'] for r in reads}!={0x2B00,0x2B01}:
        raise ValueError('incomplete first-selection directory')
    directory=max(r['half'] for r in reads)
    roots=[r['half'] for r in data['selection_traversal_reads'] if r['request']==1 and
           r['address'] in (0x2B10,0x2B11) and directory<=r['half']<first]
    if not roots:raise ValueError('missing first-selection root')
    root=min(roots)
    if not request<=directory<=root<first:raise ValueError('invalid startup anchor ordering')
    second=result['request_half']
    operands=result['operand_reads']
    before=[r for r in operands if r['half']<second]
    unknown=len(operands)>1 or any(r['half']==second for r in operands)
    notes=[e['pitch'] for e in data['events'] if e['register']==0x4C and e['half']<second]
    return dict(**result,startup=dict(request_to_directory_ms=(directory-request)/2048,
        directory_to_root_ms=(root-directory)/2048,root_to_keyon_ms=(first-root)/2048,
        request_to_keyon_ms=(first-request)/2048),
        request_spacing_ms=(second-request)/2048,notes_before_request=notes,
        tempo_read_before_request=None if unknown else bool(before),
        pre_request_boundary_ambiguous=unknown or any(e['register']==0x4C and e['half']==second for e in data['events']))


def calibrate(original,same_gap,earlier_gap):
    # Only old-track age and a measured one-frame fixture step enter the fit.
    # No wait, reply, changed-tempo or holdout result may select this reduction.
    ages=[x['prior_keyon_age_ms'] for x in (original,same_gap,earlier_gap)]
    if any(type(n) not in (int,float) or not math.isfinite(n) or not 0<n<10000 for n in ages):
        raise ValueError('invalid calibration ages')
    step=ages[1]-ages[2]
    if not 10<=step<=25:raise ValueError('calibration has no bounded one-frame step')
    reduction=math.floor((ages[1]-ages[0])/step+.5)
    if not 0<=reduction<=9:raise ValueError('calibrated request reduction outside bounds')
    return dict(request_gap_reduction=reduction,measured_frame_step_ms=step,
                original_age_ms=ages[0],same_gap_replacement_age_ms=ages[1],earlier_replacement_age_ms=ages[2],
                fitted_on='unchanged_tempo_first_keyon_age_only')


def adjusted_fixture(case,reduction):
    if type(reduction) is not int or not 0<=reduction<=9:raise ValueError('invalid request reduction')
    fixture(case)  # exact original coordinate validation
    _,_,live,gap,spin=case;gap-=reduction
    if not 3<=gap<=12:raise ValueError('calibrated fixture outside supported request gaps')
    image=build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,selection_spin=spin,prefix_duration=8)
    if image[0x4000:]!=fixture(case)[0x4000:]:raise ValueError('phase adjustment changed owned payload')
    return image


def compare(original,replacement):
    diagnostic=retry_compare(original,replacement)
    state=(not original['pre_request_boundary_ambiguous'] and not replacement['pre_request_boundary_ambiguous'] and
           original['notes_before_request']==replacement['notes_before_request'] and
           original['tempo_read_before_request'] is not None and
           original['tempo_read_before_request'] is replacement['tempo_read_before_request'])
    aligned=diagnostic['prior_keyon_age_difference_ms']<=PHASE_TOLERANCE_MS and state
    return dict(**diagnostic,pre_request_score_state_equal=state,phase_aligned=aligned,
                phase_controlled_wait_agreement=diagnostic['wait_within_2ms'] if aligned else None)


def corpus(rows,calibration):
    expected=set(cases());seen=set();groups={g:[] for g in ('baseline','holdout')}
    if set(calibration)!=set(CALIBRATION_GAPS):raise ValueError('incomplete phase calibration')
    for row in rows:
        key=tuple(row[k] for k in ('group','model','live','gap','spin'))
        if key not in expected or key in seen or any(type(row[k]) is not int for k in ('live','gap','spin')):
            raise ValueError('invalid phase corpus coordinate')
        seen.add(key);reduction=calibration[row['model']]['request_gap_reduction']
        if (row['original_fixture_sha256']!=hashlib.sha256(fixture(key)).hexdigest() or
                row['replacement_fixture_sha256']!=hashlib.sha256(adjusted_fixture(key,reduction)).hexdigest()):
            raise ValueError('phase corpus fixture identity mismatch')
        groups[row['group']].append(compare(row['original'],row['replacement']))
    if seen!=expected:raise ValueError('incomplete phase corpus')
    result={g:dict(cases=len(xs),aligned_cases=sum(x['phase_aligned'] for x in xs),
        aligned_wait_mismatches=sum(x['phase_aligned'] and not x['wait_within_2ms'] for x in xs),
        max_age_error_ms=max(x['prior_keyon_age_difference_ms'] for x in xs),
        handoff_profiles_equal=sum(x['handoff_profile_equal'] for x in xs)) for g,xs in groups.items()}
    return dict(passed=all(x['phase_aligned'] and x['wait_within_2ms'] for xs in groups.values() for x in xs),
                scope='independent_phase_witnesses_then_root_wait_comparison',groups=result)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-startup-phase-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            probe=root/args.probe.name;shutil.copy2(args.probe.resolve(),probe)
            programs={}
            for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom')):
                programs[model]=root/name;shutil.copy2(args.firmware_dir/name,programs[model])
            replacement=root/'replacement.rom';replacement.write_bytes(firmware(selection_wait=True))
            def run_image(image,model,live,program,label):
                game=root/(label+'.gb');game.write_bytes(image)
                return observe(capture(probe,program,game,model,300000000,inputs,tempo_publication=True),live)
            def training(model):
                gap=CALIBRATION_GAPS[model]
                def game(g):return build(128,live_tempo=128,change_after=3,boundary=True,request_gap=g,prefix_duration=8)
                a=run_image(game(gap),model,128,programs[model],model+'-cal-original')
                b=run_image(game(gap),model,128,replacement,model+'-cal-same')
                c=run_image(game(gap-1),model,128,replacement,model+'-cal-earlier')
                return model,dict(**calibrate(a,b,c),original=a,replacement=b,earlier_replacement=c)
            with ThreadPoolExecutor(max_workers=2) as pool:calibration=dict(pool.map(training,CALIBRATION_GAPS))
            def run(case):
                group,model,live,gap,spin=case;label=f'{group}-{model}-{live}-{gap}-{spin}'
                a=fixture(case);b=adjusted_fixture(case,calibration[model]['request_gap_reduction'])
                original=run_image(a,model,live,programs[model],label+'-original')
                owned=run_image(b,model,live,replacement,label+'-replacement')
                return dict(group=group,model=model,live=live,gap=gap,spin=spin,
                    original_fixture_sha256=hashlib.sha256(a).hexdigest(),replacement_fixture_sha256=hashlib.sha256(b).hexdigest(),
                    original=original,replacement=owned,comparison=compare(original,owned))
            with ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(run,cases()))
            report=dict(schema='gbb-sgb-vendor-startup-phase-v1',qualification=False,parameters_fitted=True,
                fitted_parameters=['replacement_request_gap_frames'],
                holdout_scope='new_request_offsets_not_used_in_age_calibration',
                probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                program_sha256={m:hashlib.sha256(p.read_bytes()).hexdigest() for m,p in programs.items()},
                replacement_sha256=hashlib.sha256(replacement.read_bytes()).hexdigest(),
                calibration=calibration,runs=rows,gate=corpus(rows,calibration))
        print(json.dumps(report))
        return 0 if report['gate']['passed'] else 1
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded startup-phase comparison failed; child diagnostics suppressed\n')


if __name__=='__main__':raise SystemExit(main())
