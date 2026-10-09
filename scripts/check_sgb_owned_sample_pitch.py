#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded PCM pitch measurements for independently authored SGB sample assets."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
from build_sgb_owned_sample_fixture import calibrated_assets
from check_sgb_instrument_chromatic_reference import PITCHES

RATE=32000
FRAMES=8192
WARMUP=2048
TOLERANCE_CENTS=10
NOTES=tuple(range(24,37))


def stimulus(asset,slot,pitch,envelope='gain'):
    if not isinstance(asset,bytes) or len(asset)!=192 or type(slot) is not int or slot not in (2,3) or (
            type(pitch) is not int or not 1<=pitch<=0x3fff) or envelope not in ('gain','adsr'):
        raise ValueError('invalid bounded sample stimulus')
    lines=[f'ram {0x5000+i} {value}' for i,value in enumerate(asset)]
    index=slot-2
    lines += [f'ram {0x2800+4*slot+i} {value}' for i,value in enumerate(asset[8+4*index:12+4*index])]
    base=slot*16
    adsr=asset[1+4*index:4+4*index] if envelope=='adsr' else (0,0,0x60)
    registers={0x5d:0x28,0x0c:0x60,0x1c:0x60,0x6c:0x20,
               0x2d:0,0x3d:0,0x4d:0,0x5c:0,
               base:0x60,base+1:0x60,base+2:pitch&255,base+3:pitch>>8,
               base+4:slot,base+5:adsr[0],base+6:adsr[1],base+7:adsr[2],0x4c:1<<slot}
    lines += [f'reg {address} {value}' for address,value in registers.items()]
    lines += ['clock 65536']*4
    return ('\n'.join(lines)+'\n').encode()


def render(runner,fixture):
    child=subprocess.run([str(runner)],input=fixture,capture_output=True,timeout=30)
    if child.returncode or len(child.stdout)!=FRAMES*4 or child.stderr:
        raise ValueError('PCM runner failed or returned incomplete bounded capture')
    samples=list(struct.iter_unpack('<hh',child.stdout))
    if any(left!=right for left,right in samples):
        raise ValueError('unexpected stereo mismatch')
    return [left for left,_ in samples],hashlib.sha256(child.stdout).hexdigest()


def audible_pitch(samples):
    """Measure known single-positive-crossing waves; reject irregular periods.

    This is deliberately not a general pitch detector for arbitrary BRR timbres.
    Flat zero runs yield only one rising crossing. End-to-end interpolation
    averages sample-grid quantization over at least twelve complete periods.
    """
    if not isinstance(samples,list) or len(samples)!=FRAMES or any(
            type(value) is not int or not -32768<=value<=32767 for value in samples):
        raise ValueError('invalid PCM capture')
    values=samples[WARMUP:]
    if min(values)>=-100 or max(values)<=100 or min(values)==-32768 or max(values)==32767:
        raise ValueError('silent, unipolar or clipped calibration capture')
    crossings=[i-1-values[i-1]/(values[i]-values[i-1]) for i in range(1,len(values))
               if values[i-1]<0<=values[i]]
    if len(crossings)<13:
        raise ValueError('insufficient calibration periods')
    intervals=[b-a for a,b in zip(crossings,crossings[1:])]
    period=(crossings[-1]-crossings[0])/(len(crossings)-1)
    if not 32<=period<=256 or max(abs(value-period) for value in intervals)>1:
        raise ValueError('irregular or out-of-range calibration periods')
    return {'frequency_hz':RATE/period,'period_samples':period,'periods':len(intervals),
            'maximum_period_deviation_samples':max(abs(value-period) for value in intervals)}


def measure(runner):
    asset=calibrated_assets()
    rows=[]
    for slot in (2,3):
        for envelope in ('gain','adsr'):
            for note,pitch in zip(NOTES,PITCHES[2]):
                pcm,digest=render(runner,stimulus(asset,slot,pitch,envelope))
                observed=audible_pitch(pcm)
                target=440*2**((note-24-9)/12)
                cents=1200*math.log2(observed['frequency_hz']/target)
                if abs(cents)>TOLERANCE_CENTS:
                    raise ValueError(f'owned sample pitch exceeds {TOLERANCE_CENTS} cents: slot={slot} note={note}')
                rows.append({'slot':slot,'instrument_id':2 if slot==2 else 10,'envelope':envelope,
                             'base_note':note,'tuning_selector':0,'pitch':pitch,'target_hz':target,
                             'error_cents':cents,'pcm_sha256':digest,**observed})
    return {'schema':'gbb-sgb-owned-sample-pitch-v1','qualification':False,'playback':False,
            'evidence':'isolated_DSP_owned_periodic_samples','sample_rate_hz':RATE,
            'frames_per_capture':FRAMES,'warmup_frames':WARMUP,'tolerance_cents':TOLERANCE_CENTS,
            'asset_sha256':hashlib.sha256(asset).hexdigest(),'observations':rows,
            'maximum_absolute_error_cents':max(abs(row['error_cents']) for row in rows)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner',type=Path,required=True)
    args=parser.parse_args()
    try:
        report=measure(args.runner.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
