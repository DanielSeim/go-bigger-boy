#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screen an independent persistent accumulator against frozen timing reports.

This is a diagnostic model, not firmware or a recovered original algorithm.
Only the baseline selects parameters; held-out observations never tune them.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from build_sgb_vendor_selection_wait_fixture import build
from check_sgb_vendor_timer_tempo_reference import TEMPO_POINTS

PULSE_HALVES=4096  # Hypothesized 2 ms hardware opportunities.
HANDLING_HALVES=274  # Fixed 0.133789 ms observable handling allowance.
ERROR_HALVES=4096  # Preserve the existing 2 ms accuracy bound.


@dataclass
class Accumulator:
    tempo: int
    phase: int

    def __post_init__(self):
        if type(self.tempo) is not int or not 1<=self.tempo<=255 or type(self.phase) is not int or not 0<=self.phase<256:
            raise ValueError('invalid accumulator parameters')

    def advance(self,ticks):
        if type(ticks) is not int or ticks<0:
            raise ValueError('invalid elapsed opportunities')
        carries,self.phase=divmod(self.phase+ticks*self.tempo,256)
        return carries

    def until_carry(self):
        return (256-self.phase+self.tempo-1)//self.tempo


def integer(value,low,high):
    if type(value) is not int or not low<=value<=high:
        raise ValueError('invalid bounded integer')
    return value


def corpus(report,schedule):
    if (not isinstance(report,dict) or report.get('schema')!='gbb-sgb-vendor-timer-tempo-v1' or
            report.get('qualification') is not False or report.get('timer_continuity') is not True or
            type(report.get('first_duration')) is not int or report['first_duration']!=96 or
            type(report.get('completed_frames')) is not int or report['completed_frames']!=124 or
            tuple(integer(report.get(k),0,2048) for k in ('request_offset','first_request_offset','selection_spin'))!=schedule):
        raise ValueError('unexpected frozen schedule')
    programs=report.get('program_sha256')
    if (not isinstance(programs,dict) or set(programs)!={'sgb1.program.rom','sgb2.program.rom'} or
            any(not isinstance(h,str) or len(h)!=64 or any(c not in '0123456789abcdef' for c in h) for h in programs.values())):
        raise ValueError('missing program identity')
    runs=report.get('runs')
    if not isinstance(runs,list) or len(runs)!=24:
        raise ValueError('incomplete frozen corpus')
    cases={}
    for run in runs:
        model=run['model'];tempo=integer(run['tempo'],1,255);state=run['state'];key=(model,tempo,state)
        if model not in ('sgb','sgb2') or tempo not in TEMPO_POINTS or state not in ('active','completed') or key in cases:
            raise ValueError('invalid or repeated model/tempo/state')
        image=build(tempo,state,first_duration=96,completed_frames=124,
                    request_offset=schedule[0],first_request_offset=schedule[1],selection_spin=schedule[2])
        if run['fixture_sha256']!=hashlib.sha256(image).hexdigest():
            raise ValueError('authored fixture identity changed')
        request=integer(run['request_half'],1,2000000000)
        directory=integer(run['directory_half'],request,request+204800)
        root=integer(run['root_half'],directory,directory+204800)
        if type(run['root_wait_ms']) not in (int,float) or run['root_wait_ms']!=(root-directory)/2048:
            raise ValueError('root anchors disagree')
        windows=run['continuity']['windows']
        if not isinstance(windows,list) or len(windows)!=3:
            raise ValueError('missing continuity windows')
        prior=windows[0]
        if integer(prior['request'],1,1)!=1 or integer(prior['end_half'],request,request)!=request:
            raise ValueError('prior request ownership changed')
        keyon=integer(prior['keyon_half'],1,request-1)
        completion=integer(prior['completion_half'],0,request)
        if bool(completion)!=(state=='completed') or completion and not keyon<completion<request:
            raise ValueError('prior playback state changed')
        anchors=[]
        for stage,low,high in (('playback',keyon,completion or request),('idle',completion or request,request)):
            access=prior[stage][0]
            integer(access['address'],253,253)
            positive=access['nonzero'];count=integer(positive['count'],0,1000000)
            last=integer(positive['last'],low if count else 0,high if count else 0)
            if count: anchors.append(last)
        if not anchors:
            raise ValueError('missing prior hardware phase anchor')
        cases[key]=dict(model=model,tempo=tempo,state=state,keyon=keyon,
                        anchor=max(anchors),directory=directory,observed=root-directory)
    return list(cases.values())


def predict(case,seed,guard):
    integer(seed,0,255);integer(guard,0,16)
    anchor=case['anchor'];directory=case['directory']
    opportunities=(directory-anchor)//PULSE_HALVES-(case['keyon']-anchor)//PULSE_HALVES
    accumulator=Accumulator(case['tempo'],seed)
    accumulator.advance(opportunities)  # Includes idle; completion never resets it.
    first=anchor+((directory-anchor)//PULSE_HALVES+1)*PULSE_HALVES
    return first-directory+PULSE_HALVES*(guard+accumulator.until_carry()-1)+HANDLING_HALVES


def evaluate(cases,seed,guard):
    rows=[]
    for case in cases:
        predicted=predict(case,seed,guard);error=abs(predicted-case['observed'])
        rows.append(dict(model=case['model'],tempo=case['tempo'],state=case['state'],
                         observed_ms=case['observed']/2048,predicted_ms=predicted/2048,
                         absolute_error_ms=error/2048,within_2ms=error<=ERROR_HALVES))
    return dict(max_error_ms=max(r['absolute_error_ms'] for r in rows),
                passing=sum(r['within_2ms'] for r in rows),total=len(rows),runs=rows)


def compare(baseline,holdout):
    training=corpus(baseline,(0,0,0));testing=corpus(holdout,(2,2,1536))
    if baseline['program_sha256']!=holdout['program_sha256']:
        raise ValueError('reference programs changed between schedules')
    def objective(seed,guard):
        errors=[abs(predict(c,seed,guard)-c['observed']) for c in training]
        return max(errors),sum(errors),seed,guard
    _,_,seed,guard=min(objective(seed,guard) for seed in range(256) for guard in range(17))
    fitted=evaluate(training,seed,guard);heldout=evaluate(testing,seed,guard)
    return dict(schema='gbb-sgb-vendor-accumulator-model-v1',qualification=False,
                program_sha256=baseline['program_sha256'],
                parameters=dict(seed=seed,guard_opportunities=guard,pulse_halves=PULSE_HALVES,
                                handling_halves=HANDLING_HALVES,modulus=256),
                training_schedule=[0,0,0],heldout_schedule=[2,2,1536],
                baseline=fitted,heldout=heldout,
                accuracy_passed=all(r['within_2ms'] for r in fitted['runs']+heldout['runs']))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--holdout',type=Path,required=True)
    args=parser.parse_args()
    try:
        reports=[]
        for path in (args.baseline,args.holdout):
            with path.open('rb') as stream: content=stream.read(1048577)
            if len(content)>1048576: raise ValueError('report too large')
            reports.append(json.loads(content))
        print(json.dumps(compare(*reports)))
    except (OSError,ValueError,KeyError,TypeError,IndexError):
        parser.exit(2,'bounded accumulator experiment failed; input diagnostics suppressed\n')


if __name__=='__main__': main()
