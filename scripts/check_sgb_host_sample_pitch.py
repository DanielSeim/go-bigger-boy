#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure owned octave pitch from real native whole-host PCM windows."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_host_sample_pitch_fixture import build,objects
from build_sgb_score_transport import build as program
from check_sgb_instrument_chromatic_reference import PITCHES,SETUPS
from check_sgb_owned_sample_pitch import audible_pitch,TOLERANCE_CENTS

IMAGE_HASH='1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82'
OPTIONS=dict.fromkeys(('multisong','uploaded_instrument','two_instruments','multiblock','instrument_profiles',
    'one_shot','brr_profiles','relocatable','atomic_upload','upload_recovery','instrument_mapping',
    'instrument_tuning','instrument_envelope','initial_score_tick','peer_gate_timing'),True)
FRAMES=2048
WARMUP=128


def fnv(data):
    value=14695981039346656037
    for byte in data:
        value=((value^byte)*1099511628211)&((1<<64)-1)
    return value


def observe(result,voice,instrument,reversed_map):
    score,asset=objects(voice,instrument,reversed_map)
    expected={'schema':'gbb-score-transport-v1','qualification':False,'playback':False,
              'reset_equal':True,'restore_equal':True,'status':1,'version':0xDA,'error':0,'external':0,
              'transfers':1,'adoptions':2,'bridge':2,'signature':0xA5,'selected_song':1,
              'admitted_roots':3,'score_tick':208,'score_hash':fnv(score),'asset_hash':fnv(asset)}
    if not isinstance(result,dict) or any(type(result.get(key)) is not type(value) or result[key]!=value
                                         for key,value in expected.items()) or type(result.get('restore_count')) is not int or (
            not 1<=result['restore_count']<=4096):
        raise ValueError('invalid native whole-host pitch metadata')
    acoustic=result.get('acoustic')
    if not isinstance(acoustic,dict) or type(acoustic.get('sample_rate_hz')) is not int or acoustic['sample_rate_hz']!=32000 or (
            type(acoustic.get('frames_per_window')) is not int or acoustic['frames_per_window']!=FRAMES):
        raise ValueError('invalid native PCM clock/window metadata')
    windows,notes=acoustic.get('windows'),result.get('envelope_notes')
    if not isinstance(windows,list) or len(windows)!=13 or not isinstance(notes,list) or len(notes)!=13:
        raise ValueError('require exactly thirteen native pitch windows and notes')
    slot=((10,2) if reversed_map else (2,10)).index(instrument)+2
    rows=[]
    previous=-1
    for index,(window,note,pitch) in enumerate(zip(windows,notes,PITCHES[2])):
        if not isinstance(window,dict) or any(type(window.get(key)) is not int or window[key]!=value for key,value in
                (('voice',voice),('slot',slot),('pitch',pitch))) or type(window.get('on_sample')) is not int or (
                not previous<window['on_sample']<=250000) or not isinstance(note,list) or len(note)!=30 or any(
                type(value) is not int for value in note) or note[:7]!=[voice,slot,*SETUPS[instrument][1:],0,pitch] or (
                note[17]<=note[16]+64*(FRAMES+2)) or note[15]!=0:
            raise ValueError('wrong voice/source/pitch or unordered/gated PCM window')
        previous=window['on_sample']
        measured=audible_pitch(window.get('pcm'),frames=FRAMES,warmup=WARMUP)
        target=440*2**((index-9)/12)
        cents=1200*math.log2(measured['frequency_hz']/target)
        if abs(cents)>TOLERANCE_CENTS:
            raise ValueError(f'whole-host pitch exceeds tolerance: voice={voice} instrument={instrument} note={24+index}')
        digest=hashlib.sha256(struct.pack('<'+'h'*FRAMES,*window['pcm'])).hexdigest()
        rows.append({'base_note':24+index,'voice':voice,'slot':slot,'pitch':pitch,
                     'on_sample':window['on_sample'],'target_hz':target,'error_cents':cents,
                     'pcm_sha256':digest,**measured})
    return rows


def run(probe,host,game,model,voice,instrument,reversed_map,mode):
    game.write_bytes(build(voice,instrument,reversed_map))
    child=subprocess.run([str(probe),str(host),str(game),model,'70000000',mode],
                         capture_output=True,text=True,timeout=90)
    if child.returncode or len(child.stdout)>256*1024:
        raise ValueError('bounded native whole-host PCM probe failed')
    result=json.loads(child.stdout)
    rows=observe(result,voice,instrument,reversed_map)
    return result,{'model':model,'voice':voice,'instrument':instrument,'reversed_map':reversed_map,'mode':mode,
                   'restore_count':result['restore_count'],'reset_equal':True,'restore_equal':True,
                   'score_sha256':hashlib.sha256(objects(voice,instrument,reversed_map)[0]).hexdigest(),
                   'asset_sha256':hashlib.sha256(objects(voice,instrument,reversed_map)[1]).hexdigest(),
                   'pcm':result['pcm'],'observations':rows}


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:
        raise ValueError('DA program image changed')
    runs=[]
    with tempfile.TemporaryDirectory(prefix='gbb-host-sample-pitch-') as directory:
        host,game=Path(directory)/'host.rom',Path(directory)/'game.gb'
        host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for instrument in (2,10):
                    for reversed_map in (False,True):
                        result,summary=run(probe,host,game,model,voice,instrument,reversed_map,'native')
                        runs.append(summary)
                        if (voice,instrument,reversed_map)==(3,10,True):
                            scalar,scalar_summary=run(probe,host,game,model,voice,instrument,reversed_map,'scalar')
                            if any(result[key]!=scalar[key] for key in ('pcm','acoustic','envelope_notes')):
                                raise ValueError('native/scalar whole-host pitch PCM differs')
                            runs.append(scalar_summary)
    return {'schema':'gbb-sgb-host-sample-pitch-v1','qualification':False,'playback':False,
            'evidence':'native_whole_host_owned_periodic_samples','image_sha256':IMAGE_HASH,
            'sample_rate_hz':32000,'frames_per_window':FRAMES,'warmup_frames':WARMUP,
            'tolerance_cents':TOLERANCE_CENTS,'runs':runs,
            'maximum_absolute_error_cents':max(abs(note['error_cents']) for row in runs for note in row['observations'])}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:
        report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps(report,indent=2))
