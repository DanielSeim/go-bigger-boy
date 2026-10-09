#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded register-only asynchronous envelope observations on private originals."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from build_sgb_async_envelope_fixture import build, CASES, peer_notes
from check_sgb_phrase_reference import run as phrase_run, MAX_ROWS
from check_sgb_instrument_chromatic_reference import SETUPS, PITCHES
from check_sgb_instrument_envelope_reference import WINDOW, POINTS, expected_points


def observe(source,case,held_voice):
    expected = peer_notes(case)
    if type(held_voice) is not int or held_voice not in (2,3):
        raise ValueError('invalid held voice')
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind','master_clock','spc_cycle','pcm_sample','address','value']:
        raise ValueError('incompatible asynchronous envelope trace header')
    registers,nodes,samples = {},{2:[],3:[]},{2:[],3:[]}
    anchor,previous,held_writes = None,-1,0
    for count,row in enumerate(reader,1):
        if count >= MAX_ROWS:
            raise ValueError('asynchronous envelope trace reached row bound')
        if row['kind'] not in ('D','E'):
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid asynchronous envelope event')
        try:
            cycle,sample,address,value = (int(row[key]) for key in ('spc_cycle','pcm_sample','address','value'))
        except (TypeError,ValueError):
            raise ValueError('invalid asynchronous envelope event') from None
        if cycle < previous or cycle < 0 or sample < 0 or not 0 <= address < 128 or not 0 <= value < 256:
            raise ValueError('unordered or invalid asynchronous envelope event')
        previous = cycle
        if row['kind'] == 'E':
            if anchor is None or address not in (0x28,0x38) or value > 127:
                raise ValueError('unanchored or invalid ENVX observation')
            values = samples[address//16]
            if len(values) >= WINDOW or sample != anchor[1]+len(values)+1 or (
                    values and cycle-values[-1][0] != 32) or (not values and not 0 < cycle-anchor[0] <= 32):
                raise ValueError('missing, duplicate or unordered ENVX sample')
            values.append((cycle,sample,value))
            continue
        held = nodes[held_voice]
        if held and held[-1]['off'] is None and address in range(16*held_voice+2,16*held_voice+8):
            held_writes += 1
        registers[address] = value
        if address == 0x4C and value:
            if value & ~12 or (anchor is None and value != 12) or registers.get(0x3D,12) & 12:
                raise ValueError('unexpected asynchronous voices or noise')
            if anchor is None:
                anchor = (cycle,sample)
            for voice in (2,3):
                if not value & (1<<voice):
                    continue
                index = len(nodes[voice])
                sequence = ((10,24),) if voice == held_voice else expected
                if index >= len(sequence):
                    raise ValueError('unexpected asynchronous onset count')
                instrument,note = sequence[index]
                base = 16*voice
                if any(base+offset not in registers for offset in range(2,8)):
                    raise ValueError('incomplete asynchronous setup')
                pitch = registers[base+2] | registers[base+3]<<8
                descriptor = tuple(registers[base+offset] for offset in range(4,8))
                if pitch != PITCHES[instrument][note-24] or descriptor != SETUPS[instrument]:
                    raise ValueError('asynchronous descriptor/source/pitch differs')
                nodes[voice].append({'voice':voice,'instrument':instrument,'base_note':note,
                    'on':cycle,'sample':sample,'off':None,'pitch':pitch,
                    **dict(zip(('srcn','adsr1','adsr2','gain'),descriptor))})
        if address == 0x5C:
            for voice in (2,3):
                if value & (1<<voice) and nodes[voice] and nodes[voice][-1]['off'] is None:
                    nodes[voice][-1]['off'] = cycle
    if anchor is None or any(len(values) != WINDOW for values in samples.values()) or any(
            len(nodes[voice]) != (1 if voice == held_voice else len(expected)) for voice in (2,3)):
        raise ValueError('incomplete asynchronous fixture/window')
    if [(c,s) for c,s,_ in samples[2]] != [(c,s) for c,s,_ in samples[3]]:
        raise ValueError('peer sample clocks differ')
    summaries = []
    for voice in (2,3):
        for index,node in enumerate(nodes[voice]):
            end = nodes[voice][index+1]['on'] if index+1 < len(nodes[voice]) else samples[voice][-1][0]+1
            off = node['off']
            if off is None or not node['on'] < off < end:
                raise ValueError('missing or unordered asynchronous key-off')
            active = [row for row in samples[voice] if node['on'] < row[0] < off]
            released = [row for row in samples[voice] if off <= row[0] < end]
            if not active or not released:
                raise ValueError('missing active/released ENVX observations')
            peak = max(row[2] for row in active)
            peak_index = next(i for i,row in enumerate(active) if row[2] == peak)
            if active[0][2] != 0 or peak != 127 or any(b[2]>a[2] for a,b in zip(
                    active[peak_index:],active[peak_index+1:])) or any(b[2]>a[2] for a,b in zip(released,released[1:])):
                raise ValueError('unexpected asynchronous attack, decay or release')
            drops = [(b[1],a[2]-b[2]) for a,b in zip(released,released[1:]) if b[2]<a[2]]
            zero = next((row for row in released if row[2]==0),None)
            if not drops or any(drop!=1 for _,drop in drops) or any(b[0]-a[0]!=2 for a,b in zip(drops,drops[1:])):
                raise ValueError('unexpected asynchronous release step/cadence')
            if zero is None or any(row[2] for row in released if row[1]>=zero[1]):
                raise ValueError('asynchronous release did not settle before retrigger/window end')
            summaries.append({key:node[key] for key in ('voice','instrument','base_note','pitch','srcn','adsr1','adsr2','gain')} | {
                'onset_spc_cycles':node['on']-anchor[0], 'gate_spc_cycles':off-node['on'],
                'peak':peak,'peak_sample_offset':active[peak_index][1]-node['sample'],
                'active_last':active[-1][2], 'active_points':{str(sample-node['sample']):value for _,sample,value in active
                    if sample-node['sample'] in POINTS},
                'release_start':released[0][2],'release_drops':len(drops),'release_drop_samples':2,
                'release_zero':True,'release_zero_spc_cycles':zero[0]-off})
    return {'envelopes':summaries,'held_setup_writes':held_writes,'sample_count_per_voice':WINDOW}


def run(trace,firmware_dir,model,case,held_voice):
    result = phrase_run(trace,firmware_dir,model,case,
        fixture_builder=lambda name:build(name,held_voice),
        observer=lambda source:observe(source,case,held_voice),
        contract=lambda result,name:contract(result,name,held_voice),
        pattern_durations=None,trace_options=('--voice-envelope-trace',))
    return dict(result,held_voice=held_voice,instruction_limit=8000000)


def contract(result,case,held_voice):
    sequence = peer_notes(case)
    if type(held_voice) is not int or held_voice not in (2,3):
        raise ValueError('invalid held voice contract')
    if type(result.get('held_setup_writes')) is not int or result['held_setup_writes'] != 0 or type(
            result.get('sample_count_per_voice')) is not int or result['sample_count_per_voice'] != WINDOW:
        raise ValueError('held setup changed or sample window differs')
    envelopes = result.get('envelopes')
    if not isinstance(envelopes,list) or len(envelopes) != 1+len(sequence):
        raise ValueError('incomplete asynchronous envelope contract')
    index = 0
    for voice in (2,3):
        held = voice == held_voice
        for note_index,(instrument,note) in enumerate(((10,24),) if held else sequence):
            env = envelopes[index]
            index += 1
            setup = {'voice':voice,'instrument':instrument,'base_note':note,'pitch':PITCHES[instrument][note-24],
                     **dict(zip(('srcn','adsr1','adsr2','gain'),SETUPS[instrument]))}
            points = expected_points(instrument,'held' if held and case!='clipped' else
                                     'short' if held else 'retrigger',24 if instrument==10 else 25)
            shifted = not held and ((case=='retrigger' and note_index==1) or (case=='switch' and note_index==2))
            if shifted:
                points = expected_points(10,'retrigger',25)
            peak_offset = 10 if instrument==2 else 198 if shifted else 197
            last = (92 if case=='clipped' else 70) if held else (109 if note_index==3 else 110) if instrument==2 else 111
            tick = 0 if held else note_index*(32 if case=='rests' else 16)
            onset_bounds = {0:(0,0),16:(84000,86000),32:(171000,174000),48:(257000,260000)}[tick]
            gate_bounds = ((170000,172000) if case=='clipped' else (334000,336000)) if held else (
                (72000,74000) if note_index==0 else (75000,78000) if note_index==3 else (73000,75000))
            fixed = {**setup,'peak':127,'peak_sample_offset':peak_offset,'active_last':last,'active_points':points,
                     'release_start':last,'release_drops':last,'release_drop_samples':2,'release_zero':True}
            timing = {'onset_spc_cycles':onset_bounds,'gate_spc_cycles':gate_bounds,
                      'release_zero_spc_cycles':(64*last-2,64*last+128)}
            if not isinstance(env,dict) or set(env) != set(fixed)|set(timing) or any(
                    env[key] != value for key,value in fixed.items()) or any(
                    type(env[key]) is not int for key in (set(fixed)|set(timing))-{'active_points','release_zero'}) or (
                    env['release_zero'] is not True) or not isinstance(env['active_points'],dict) or any(
                    type(value) is not int for value in env['active_points'].values()) or any(
                    not low <= env[key] <= high for key,(low,high) in timing.items()):
                raise ValueError('asynchronous measured trajectory contract differs')


def check_models(results):
    required = {(model,case,voice) for model in ('sgb','sgb2') for case in CASES for voice in (2,3)}
    if not isinstance(results,list) or len(results)!=16 or any(not isinstance(row,dict) or type(
            row.get('held_voice')) is not int or row.get('model') not in ('sgb','sgb2') or type(
            row.get('case')) is not str or row['case'] not in CASES for row in results) or {
            (row['model'],row['case'],row['held_voice']) for row in results} != required:
        raise ValueError('require every asynchronous case/orientation on both models')
    by_key = {(row['model'],row['case'],row['held_voice']):row for row in results}
    for row in results:
        contract(row,row['case'],row['held_voice'])
    for case in CASES:
        for voice in (2,3):
            first,second = [by_key[model,case,voice] for model in ('sgb','sgb2')]
            for a,b in zip(first['envelopes'],second['envelopes']):
                for key in a:
                    tolerance = 1024 if key in ('onset_spc_cycles','gate_spc_cycles') else 128 if key=='release_zero_spc_cycles' else 0
                    different = abs(a[key]-b[key]) > tolerance if tolerance else a[key]!=b[key]
                    if different:
                        raise ValueError('two-model asynchronous envelope differs')
    for model in ('sgb','sgb2'):
        for voice in (2,3):
            held = [next(note for note in by_key[model,case,voice]['envelopes'] if note['voice']==voice) for case in CASES]
            for peer in held[1:3]:
                if any(peer[key]!=held[0][key] for key in ('peak_sample_offset','active_points','active_last','release_start')) or abs(
                        peer['gate_spc_cycles']-held[0]['gate_spc_cycles'])>1024:
                    raise ValueError('peer activity disturbed held envelope')
            if any(held[3]['active_points'][key]!=value for key,value in held[0]['active_points'].items() if key!='8192') or not (
                    held[3]['gate_spc_cycles'] < held[0]['gate_spc_cycles']):
                raise ValueError('clipped held prefix or early release differs')


def dsp_window(source):
    """Digest bounded DSP writes for an asynchronous sampler-on/off witness."""
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind','master_clock','spc_cycle','pcm_sample','address','value']:
        raise ValueError('incompatible asynchronous DSP witness header')
    anchor,last_sample,previous,writes,onsets = None,0,-1,[],0
    for count,row in enumerate(reader,1):
        if count>=MAX_ROWS:
            raise ValueError('asynchronous DSP witness reached row bound')
        if row['kind'] not in ('D','E'):
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid asynchronous DSP witness event')
        try:
            cycle,sample,address,value = (int(row[key]) for key in ('spc_cycle','pcm_sample','address','value'))
        except (TypeError,ValueError):
            raise ValueError('invalid asynchronous DSP witness event') from None
        if cycle<previous or cycle<0 or sample<0 or not 0<=address<128 or not 0<=value<256:
            raise ValueError('invalid or unordered asynchronous DSP witness event')
        previous,last_sample = cycle,sample
        if anchor is not None and sample>anchor+WINDOW:
            break
        if row['kind']!='D':
            continue
        if address==0x4C and value:
            if (anchor is None and value!=12) or value not in (4,8,12):
                raise ValueError('unexpected asynchronous DSP witness onset')
            if anchor is None:
                anchor=sample
            onsets+=1
        writes.append((cycle,sample,address,value))
    if anchor is None or last_sample<anchor+WINDOW or onsets!=4:
        raise ValueError('incomplete asynchronous DSP witness window/onsets')
    digest = hashlib.sha256(b''.join(struct.pack('<QQBB',*write) for write in writes)).hexdigest()
    return {'dsp_window_sha256':digest,'dsp_window_writes':len(writes)}


def noninterference(trace,firmware_dir):
    witnesses=[]
    for model in ('sgb','sgb2'):
        pair=[phrase_run(trace,firmware_dir,model,'switch',fixture_builder=lambda name:build(name,2),
                         observer=dsp_window,contract=lambda result,name:None,pattern_durations=None,
                         trace_options=options) for options in ((),('--voice-envelope-trace',))]
        fields=('dsp_window_sha256','dsp_window_writes')
        if any(pair[0][field]!=pair[1][field] for field in fields):
            raise ValueError('ENVX sampler changed asynchronous DSP write execution')
        witnesses.append({'model':model,'case':'switch','held_voice':2,'equal':True,
                          **{field:pair[0][field] for field in fields}})
    return witnesses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(),args.firmware_dir.resolve(),model,case,voice)
                   for case in CASES for voice in (2,3) for model in ('sgb','sgb2')]
        check_models(results)
        witnesses = noninterference(args.trace.resolve(),args.firmware_dir.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-sgb-async-envelope-reference-v1',
                      'qualification':False,'playback':False,'runs':results,
                      'sampler_noninterference':witnesses},indent=2))


if __name__ == '__main__':
    main()
