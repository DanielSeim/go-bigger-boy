#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure selection delay and cumulative phase without claiming compatibility."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware, ROOT
from build_sgb_vendor_phase_fixture import build, sequence, CASES
from check_sgb_vendor_music_reference import capture, timing_summary


def summarize(data, case):
    notes = [event for event in data['events'] if event['register'] == 0x4C]
    expected = sum(note is not None for _,_,note in sequence(case)[1])
    selecting = case.startswith('selection')
    requests = [i+1 for i,music in enumerate(data['music']) if music == 1]
    selections = 4 if case == 'selection-completed' else (9 if selecting else 1)
    if len(requests) != selections or any(music not in (0,1) for music in data['music']):
        raise ValueError('invalid owned selection commands')
    if len(notes) != (len(requests) if selecting else expected):
        raise ValueError('owned chain/selection lost a note')
    if [event['request'] for event in notes] != (requests if selecting else [requests[0]]*expected):
        raise ValueError('owned notes do not belong to their delivered selection')
    final = data.get('final')
    if not isinstance(final,dict) or any(type(final.get(k)) is not int or final[k] != 0
            for k in ('flg','kof','echo_left','echo_right')):
        raise ValueError('owned playback did not finish with released DSP and silent echo')
    result = dict(pitches=[event['pitch'] for event in notes],
                  selection_ms=[(event['half']-data['requests'][event['request']-1])/2048 for event in (notes if selecting else notes[:1])],
                  request_gaps_ms=[(data['requests'][b-1]-data['requests'][a-1])/2048 for a,b in zip(requests,requests[1:])],
                  final={key:final[key] for key in ('flg','kof','echo_left','echo_right')})
    if not selecting:
        result['timing'] = timing_summary(data)
        result['phase_ms'] = [(note['half']-notes[0]['half'])/2048 for note in notes]
    return result


def compare(original, replacement):
    if original['pitches'] != replacement['pitches']:
        raise ValueError('owned phase fixture pitch mismatch')
    def delta(key):
        a,b = original[key],replacement[key]
        if len(a) != len(b): raise ValueError('owned observation length mismatch')
        return [y-x for x,y in zip(a,b)]
    result = dict(selection_error_ms=delta('selection_ms'))
    if 'phase_ms' in original:
        result['phase_error_ms'] = delta('phase_ms')
        a,b = original['timing'],replacement['timing']
        result['gate_error_ms'] = [y-x for x,y in zip(a['gates_ms'],b['gates_ms'])]
        result['completion_error_ms'] = b['completion_ms']-a['completion_ms']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--model',choices=('sgb','sgb2'),action='append')
    parser.add_argument('--case',choices=CASES,action='append')
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-vendor-phase-') as directory:
            base = Path(directory)
            owned = base/'owned.rom'; owned.write_bytes(firmware())
            inputs = base/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            reports = []
            for model in args.model or ('sgb','sgb2'):
                original = args.firmware_dir/('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
                result = dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases={})
                for case in args.case or CASES:
                    image = build(case); game = base/'game.gb'; game.write_bytes(image)
                    measured = {kind:summarize(capture(args.probe.resolve(),program.resolve(),game,model,500000000,inputs),case)
                                for kind,program in (('original',original),('replacement',owned))}
                    result['cases'][case] = dict(fixture_sha256=hashlib.sha256(image).hexdigest(),
                                                **measured,difference=compare(measured['original'],measured['replacement']))
                reports.append(result)
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private phase observation failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-phase-v1',qualification=False,
                         firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=reports)))


if __name__ == '__main__': main()
