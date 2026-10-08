#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate guarded short-return envelopes, actual DSP writes and final audio."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_sgb_score_short_return import build as program_build
from build_sgb_score_short_return_fixture import bank,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_short_return_reference import agree,run,SCHEMA as REF_SCHEMA
from check_sgb_score_direct_return_playback import validate as prior_validate,compare as prior_compare,identity as prior_identity,SCHEMA as PRIOR_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_duet_reference import integer
SCHEMA='gbb-spc-score-short-return-v1'


def identity(r):return prior_identity(r,short_return=True)


def validate(r):
    if r.get('schema')!=SCHEMA:raise ValueError('invalid short-return schema')
    value=1 if r.get('status')==2 else 0
    if not integer(r.get('short_return_mode'),value,value):raise ValueError('invalid short-return mode')
    prior_validate({**r,'schema':PRIOR_SCHEMA},short_return=True,fixture_bank=bank)


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,refs):
    return prior_compare(candidates,refs,short_return=True,validator=validate,reference_agreement=agree)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',type=Path,required=True);p.add_argument('--reference',type=Path);p.add_argument('--trace',type=Path);p.add_argument('--firmware-dir',type=Path);args=p.parse_args()
    try:
        if args.reference:
            if args.reference.stat().st_size>1024*1024:raise ValueError('direct-return reference exceeds JSON bound')
            loaded=json.loads(args.reference.read_text())
            if loaded.get('schema')!=REF_SCHEMA or loaded.get('qualification') is not False or loaded.get('playback') is not False:raise ValueError('invalid direct-return reference artifact')
            refs=loaded['reference']
        else:
            if not args.trace or not args.firmware_dir:p.error('provide --reference or --trace and --firmware-dir')
            refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for a in ARTICULATIONS for d in DURATIONS for m in ('sgb','sgb2')]
        candidates={f'{c}-{a}-{d}':native(args.probe.resolve(),bank(a,d,c)) for c in CASES for a in ARTICULATIONS for d in DURATIONS}
        result={'schema':'gbb-score-short-return-playback-reference-v1','qualification':False,'playback':False,'program_sha256':hashlib.sha256(program_build()).hexdigest(),'native':candidates,'reference':refs,'comparisons':compare(candidates,refs),'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
