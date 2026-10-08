#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check bounded phrase lists, pattern transitions and private DSP evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_sgb_score_list import build
from build_sgb_list_fixture import CASES,bank,build as fixture_build
from check_sgb_score_bank_reference import envelope_report,compare as bank_compare,SCHEMA as BANK_SCHEMA
from check_sgb_score_envelope_reference import validate as envelope_validate,align as envelope_align
from check_sgb_score_mix_reference import observe,contract as mix_contract,KEYS as LEGACY_KEYS
from build_sgb_mix_fixture import NOTES,volumes
from check_sgb_score_chromatic_reference import PITCHES
from check_sgb_score_polygate_reference import native as gate_native,native_gates
from check_sgb_phrase_reference import run as phrase_run

SCHEMA='gbb-spc-score-list-v1'
KEYS=tuple(f'{case}-{art}' for case in CASES for art in (127,63))


def bank_report(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA or not isinstance(report.get('pattern_ticks'),list):
        raise ValueError('invalid phrase-list schema/entries')
    return {**report,'schema':BANK_SCHEMA}


def validate(report):
    envelope_validate(envelope_report(bank_report(report)),max_events=64,pattern_ticks=report['pattern_ticks'])


def align(report,data):
    validate(report)
    envelope_align(envelope_report(bank_report(report)),data,max_events=64,pattern_ticks=report['pattern_ticks'])


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=build,validator=validate,output_bound=131072)


def contract(result,case):
    mix_contract(result,CASES[case])


def run(trace,firmware_dir,model,articulation,case):
    result=phrase_run(trace,firmware_dir,model,case,
                      fixture_builder=lambda name:fixture_build(articulation,name),
                      observer=lambda source:observe(source,CASES[case]),contract=contract,pattern_durations=None)
    result.update(articulation=articulation,instruction_limit=8000000)
    return result


def compare(candidates,references):
    if not isinstance(candidates,dict): raise ValueError('invalid phrase-list candidates')
    # Keep the preceding owned comparator tests reusable for two-pattern inputs.
    if set(candidates)==set(LEGACY_KEYS):
        for report in candidates.values(): validate(report)
        return bank_compare({key:bank_report(value) for key,value in candidates.items()},references)
    if set(candidates)!=set(KEYS) or not isinstance(references,list) or len(references)!=8:
        raise ValueError('require both list cases/articulations/models')
    seen,comparisons=set(),[]
    for reference in references:
        if not isinstance(reference,dict): raise ValueError('invalid list reference')
        case,art,model=(reference.get(key) for key in ('case','articulation','model'))
        if case not in CASES or type(art) is not int or art not in (63,127) or model not in ('sgb','sgb2') or (case,art,model) in seen:
            raise ValueError('invalid or duplicate list reference')
        seen.add((case,art,model));contract(reference,case)
        report=candidates[f'{case}-{art}'];align(report,bank(art,case))
        if [edge['pitches'] for edge in report['keyons']]!=[[PITCHES[note]]*2 for note in NOTES] or [edge['volumes'] for edge in report['keyons']]!=volumes(CASES[case]) or any(edge['mask']!=12 for edge in report['keyons']):
            raise ValueError('native list fixture setup sequence differs')
        actual,original=native_gates(report),reference['note_gates']
        differences=[abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(actual,original)]
        if len(actual)!=16 or any(a['voice']!=b['voice'] or a['cause']!=1 for a,b in zip(actual,original)) or any(value>4096 for value in differences):
            raise ValueError('list gate outside existing allowance')
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if len(intervals)!=7 or any(abs(a-b)>4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):
            raise ValueError('list onset outside existing allowance')
        comparisons.append({'case':case,'articulation':art,'model':model,'absolute_gate_difference_spc_cycles':differences,
                            'native_DSP_intervals_spc_cycles':intervals,'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles']})
    for case in CASES:
        for art in (63,127):
            first,second=[r for r in references if r['case']==case and r['articulation']==art]
            if first['keyons']!=second['keyons'] or any(abs(a-b)>2048 for a,b in zip(first['onset_intervals_spc_cycles'],second['onset_intervals_spc_cycles'])):
                raise ValueError('list reference models disagree')
    return comparisons


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        candidates={f'{case}-{art}':native(args.probe.resolve(),bank(art,case)) for case in CASES for art in (127,63)}
        references=[run(args.trace.resolve(),args.firmware_dir.resolve(),model,art,case) for case in CASES for art in (127,63) for model in ('sgb','sgb2')]
        comparisons=compare(candidates,references)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error: parser.error(str(error))
    print(json.dumps({'schema':'gbb-score-list-reference-v1','qualification':False,'playback':False,
                      'program_sha256':hashlib.sha256(build()).hexdigest(),'native':candidates,'reference':references,'comparisons':comparisons},indent=2))


if __name__=='__main__': main()
