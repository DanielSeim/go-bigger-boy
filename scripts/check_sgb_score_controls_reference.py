#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate bounded native instrument/volume/pan register setup using owned audio."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from build_sgb_score_controls import build
from check_sgb_pitch_reference import run as run_pitch
from check_sgb_volume_pan_reference import run as run_volume
from check_sgb_score_render_reference import validate as validate_render
from check_sgb_score_track_reference import align as align_track, symbolic

INSTRUMENTS = {
    2: {'pitches': {0x98:1068,0x99:1132,0xA4:2140}, 'adsr1':0x8F,'adsr2':0x6F},
    10: {'pitches': {0x98:7993,0x99:8472,0xA4:16016}, 'adsr1':0x8E,'adsr2':0xAF},
}
PAN_VOLUMES = {
    (10,160,127):(7,7), (10,80,127):(1,1), (10,160,64):(1,1),
    (0,160,127):(0,11), (20,160,127):(11,0),
}


def validate(report):
    if not isinstance(report,dict) or report.get('schema')!='gbb-spc-score-controls-v2':
        raise ValueError('invalid native control schema')
    keyons=report.get('keyons')
    fields=('instrument','song_volume','track_volume','pan','voll','volr','srcn','adsr1','adsr2','gain')
    if not isinstance(keyons,list) or any(not isinstance(on,dict) or any(
            type(on.get(key)) is not int for key in fields) for on in keyons):
        raise ValueError('invalid native control fields')
    normalized={**report,'schema':'gbb-spc-score-render-v1'}
    validate_render(normalized,pitch_lookup=lambda on: INSTRUMENTS.get(on['instrument'],{}).get('pitches',{}).get(on['opcode']))
    if type(report.get('tempo')) is not int or not 0<=report['tempo']<=255 or (
            report['status']==2 and report['tempo']!=96):
        raise ValueError('invalid native control tempo')
    offs=report.get('keyoff_half_cycles')
    if not isinstance(offs,list) or len(offs)!=len(keyons):
        raise ValueError('native control key-off count differs')
    notes=[event for event in report['events'] if event['opcode']!=0xC9]
    for on,off,note in zip(keyons,offs,notes):
        instrument=INSTRUMENTS.get(on['instrument'])
        volume=PAN_VOLUMES.get((on['pan'],on['song_volume'],on['track_volume']))
        if instrument is None or volume is None or (on['instrument']==10 and (on['pan']!=10 or volume!=(7,7))):
            raise ValueError('unsupported native control combination')
        if (on['srcn']!=on['instrument'] or on['adsr1']!=instrument['adsr1'] or
                on['adsr2']!=instrument['adsr2'] or on['gain']!=0xB8 or
                on['voll']!=volume[0] or on['volr']!=volume[1] or note['duration']!=16):
            raise ValueError('native instrument/volume setup differs')
        following=next((event['half_cycle'] for event in report['events'] if event['tick']>note['tick']),30_000_000)
        if type(off) is not int or not on['half_cycle']<off<following:
            raise ValueError('native control gate ordering differs')
    pcm=report['pcm']
    channel_fields=('left_nonzero_frames','right_nonzero_frames','left_peak','right_peak')
    if any(type(pcm.get(key)) is not int for key in channel_fields) or type(pcm.get('stereo_equal')) is not bool:
        raise ValueError('invalid native channel PCM fields')
    left,right=pcm['left_nonzero_frames'],pcm['right_nonzero_frames']
    if (not 0<=left<=pcm['nonzero_frames'] or not 0<=right<=pcm['nonzero_frames'] or
            pcm['nonzero_frames']>left+right or not 0<=pcm['left_peak']<=32768 or
            not 0<=pcm['right_peak']<=32768 or pcm['peak']!=max(pcm['left_peak'],pcm['right_peak'])):
        raise ValueError('invalid native channel PCM ranges')
    pans={on['pan'] for on in keyons}
    if pans<={10} and (not pcm['stereo_equal'] or left!=right or left!=pcm['nonzero_frames'] or pcm['left_peak']!=pcm['right_peak']):
        raise ValueError('centered or silent stream has unequal stereo PCM')
    if pans and pans<={0} and (left!=0 or pcm['left_peak']!=0 or right!=pcm['nonzero_frames']):
        raise ValueError('right endpoint leaked PCM into the left channel')
    if pans and pans<={20} and (right!=0 or pcm['right_peak']!=0 or left!=pcm['nonzero_frames']):
        raise ValueError('left endpoint leaked PCM into the right channel')
    if (0 in pans or 20 in pans) and pcm['stereo_equal']:
        raise ValueError('endpoint stream produced equal stereo PCM')
    return report


def align(report,track):
    validate(report)
    align_track({**report,'schema':'gbb-spc-score-track-v1'},track)
    instrument,song,volume,pan=2,160,127,10
    expected=[]
    for event in symbolic(track)['events']:
        if event['kind']=='instrument': instrument=event['value']
        elif event['kind']=='song_volume': song=event['value']
        elif event['kind']=='track_volume': volume=event['value']
        elif event['kind']=='pan': pan=event['value']
        elif event['kind']=='note': expected.append((instrument,song,volume,pan))
    if expected!=[(on['instrument'],on['song_volume'],on['track_volume'],on['pan']) for on in report['keyons']]:
        raise ValueError('native inherited control state differs from symbolic oracle')


def native(probe,track,tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-controls-') as directory:
        program,stream=[Path(directory)/name for name in ('program.bin','track.bin')]
        program.write_bytes(build())
        stream.write_bytes(track)
        completed=subprocess.run([str(probe.resolve()),str(program),str(stream),str(tempo)],
                                 capture_output=True,text=True,timeout=60)
        if completed.returncode!=0 or len(completed.stdout)>32768:
            raise ValueError('native control probe failed or exceeded report bound')
        return validate(json.loads(completed.stdout))


def fixture(case):
    if case in ('instrument-2','instrument-10'):
        instrument=2 if case=='instrument-2' else 10
        return bytes((0xE0,instrument,0xE5,160,0xED,127,16,127,0xC9,0x98,0x99,0xA4,8,0xC9,0))
    if case=='volume':
        return bytes((0xE0,2,0xE5,160,0xED,127,16,127,0xC9,0x98,
                      0xE5,80,0x98,0xE5,160,0xED,64,0x98,8,0xC9,0))
    if case=='pan':
        return bytes((0xE0,2,0xE5,160,0xED,127,0xE1,10,16,127,0xC9,0x98,
                      0xE1,0,0x98,0xE1,20,0x98,8,0xC9,0))
    raise ValueError('unknown native control fixture')


def compare(candidates,references):
    cases=('instrument-2','instrument-10','volume','pan')
    if (not isinstance(references,list) or len(references)!=8 or
            any(not isinstance(row,dict) for row in references) or
            {(row.get('model'),row.get('native_case')) for row in references} != {
                (model,case) for model in ('sgb','sgb2') for case in cases} or set(candidates)!=set(cases)):
        raise ValueError('incomplete native control reference matrix')
    comparisons=[]
    for reference in references:
        case=reference['native_case']
        candidate=candidates[case]
        align(candidate,fixture(case))
        fields=('pitch','srcn','voll','volr','pan') if case=='pan' else (
            ('pitch','srcn','voll','volr') if case=='volume' else ('pitch','srcn','adsr1','adsr2','gain'))
        notes=reference.get('notes')
        if not isinstance(notes,list) or len(notes)!=3 or any(not isinstance(note,dict) for note in notes) or len(candidate['keyons'])!=3:
            raise ValueError('incomplete native/reference note setup')
        for own,original in zip(candidate['keyons'],notes):
            if any(type(original.get(key)) is not int or own[key]!=original[key] for key in fields):
                raise ValueError('native control registers differ from original note setup')
        comparisons.append({**reference,'compared_fields':list(fields),
                            'native_setup':[{key:on[key] for key in fields} for on in candidate['keyons']]})
    return comparisons


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        candidates={case:native(args.probe,fixture(case)) for case in ('instrument-2','instrument-10','volume','pan')}
        references=[]
        for model in ('sgb','sgb2'):
            for instrument in (2,10):
                references.append({**run_pitch(args.trace.resolve(),args.firmware_dir.resolve(),model,instrument),
                                   'native_case':f'instrument-{instrument}'})
            for case in ('volume','pan'):
                references.append({**run_volume(args.trace.resolve(),args.firmware_dir.resolve(),model,case),
                                   'native_case':case})
        comparisons=compare(candidates,references)
    except (OSError,ValueError,TypeError,KeyError,subprocess.TimeoutExpired) as error:
        parser.exit(1,f'native control check failed: {error}\n')
    print(json.dumps({'schema':'gbb-score-controls-reference-v2','qualification':False,'playback':False,
                      'evidence':'native_instrument_volume_and_pan_registers_with_owned_stereo_PCM',
                      'native_program_sha256':hashlib.sha256(build()).hexdigest(),
                      'owned_pcm':{case:report['pcm'] for case,report in candidates.items()},
                      'comparisons':comparisons},indent=2))


if __name__=='__main__':
    main()
