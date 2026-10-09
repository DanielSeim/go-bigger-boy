#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Retain only bounded DSP setup and published ENVX envelope summaries."""
import argparse
import csv
import hashlib
import struct
import json
from pathlib import Path
import subprocess

from build_sgb_instrument_envelope_fixture import build, CASES
from check_sgb_phrase_reference import run as phrase_run, MAX_ROWS
from check_sgb_instrument_chromatic_reference import SETUPS, PITCHES

WINDOW = 12000
POINTS = (1,4,8,16,32,64,128,512,2048,4096,8192)


def observe(source, instrument, case):
    if type(instrument) is not int or instrument not in SETUPS or type(case) is not str or case not in CASES:
        raise ValueError('unknown envelope fixture')
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind','master_clock','spc_cycle','pcm_sample','address','value']:
        raise ValueError('incompatible envelope trace header')
    registers,nodes,edges,samples = {},[],[],{2:[],3:[]}
    previous = -1
    expected_notes = CASES[case][2]
    for count,row in enumerate(reader,1):
        if count >= MAX_ROWS:
            raise ValueError('envelope trace reached row bound')
        if row['kind'] not in ('D','E'):
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid envelope event')
        try:
            cycle,sample,address,value = (int(row[key]) for key in ('spc_cycle','pcm_sample','address','value'))
        except (ValueError,TypeError):
            raise ValueError('invalid envelope event') from None
        if cycle < previous or sample < 0 or cycle < 0 or not 0 <= address < 128 or not 0 <= value < 256:
            raise ValueError('invalid or unordered envelope event')
        previous = cycle
        if row['kind'] == 'E':
            if address not in (0x28,0x38) or value > 127 or not edges:
                raise ValueError('unexpected envelope register or unanchored sample')
            voice = address//16
            values = samples[voice]
            if sample != edges[0]['sample']+len(values)+1 or len(values) >= WINDOW or (
                    values and cycle-values[-1][0] != 32):
                raise ValueError('missing, duplicate or unordered envelope samples')
            if not values and not 0 < cycle-nodes[0]['on_cycle'] <= 32:
                raise ValueError('envelope sample clock is not anchored to key-on')
            values.append((cycle,sample,value))
            continue
        registers[address] = value
        if address == 0x4C and value:
            if value != 12 or len(edges) >= len(expected_notes) or 0x3D not in registers or registers[0x3D] & 12:
                raise ValueError('unexpected envelope voices, noise or onset count')
            note = expected_notes[len(edges)]
            setup = []
            for voice in (2,3):
                base = 16*voice
                if any(base+offset not in registers for offset in range(2,8)):
                    raise ValueError('incomplete envelope setup')
                pitch = registers[base+2] | registers[base+3]<<8
                descriptor = tuple(registers[base+offset] for offset in range(4,8))
                if pitch != PITCHES[instrument][note-24] or descriptor != SETUPS[instrument]:
                    raise ValueError('envelope pitch/source/descriptor differs')
                node = {'voice':voice,'base_note':note,'on_cycle':cycle,'on_sample':sample,'off_cycle':None}
                nodes.append(node)
                setup.append({'voice':voice,'pitch':pitch,**dict(zip(('srcn','adsr1','adsr2','gain'),descriptor))})
            edges.append({'sample':sample,'voices':setup})
        if address == 0x5C and value & 12:
            for voice in (2,3):
                active = next((node for node in reversed(nodes) if node['voice'] == voice),None)
                if active and value & (1<<voice) and active['off_cycle'] is None:
                    active['off_cycle'] = cycle
    if len(edges) != len(expected_notes) or any(len(values) != WINDOW for values in samples.values()):
        raise ValueError('incomplete envelope fixture/window')
    if [(c,s) for c,s,_ in samples[2]] != [(c,s) for c,s,_ in samples[3]]:
        raise ValueError('envelope peer sample clocks differ')
    summaries = []
    for index,node in enumerate(nodes):
        voice = node['voice']
        next_on = nodes[index+2]['on_cycle'] if index+2 < len(nodes) else samples[voice][-1][0]+1
        off = node['off_cycle']
        if off is None or not node['on_cycle'] < off < next_on:
            raise ValueError('missing envelope key-off')
        values = [value for value in samples[voice] if node['on_cycle'] < value[0] < next_on]
        active = [value for value in values if value[0] < off]
        released = [value for value in values if value[0] >= off]
        if not active or not released:
            raise ValueError('missing active/released envelope observations')
        peak = max(value for _,_,value in active)
        peak_index = next(i for i,value in enumerate(active) if value[2] == peak)
        decay = [value for _,_,value in active[peak_index:]]
        release = [value for _,_,value in released]
        if active[0][2] != 0 or peak != 127 or any(b>a for a,b in zip(decay,decay[1:])) or any(
                b>a for a,b in zip(release,release[1:])):
            raise ValueError('unexpected envelope attack, decay or release trajectory')
        drops = [(released[i+1][1],a-b) for i,(a,b) in enumerate(zip(release,release[1:])) if b<a]
        if not drops or any(drop != 1 for _,drop in drops):
            raise ValueError('unexpected envelope release steps')
        intervals = [b[0]-a[0] for a,b in zip(drops,drops[1:])]
        if any(interval != 2 for interval in intervals):
            raise ValueError('unexpected envelope release cadence')
        zero = next((value for value in released if value[2] == 0),None)
        if zero is None and index+2 >= len(nodes):
            raise ValueError('final envelope release did not settle')
        if zero and any(value[2] for value in released if value[1] >= zero[1]):
            raise ValueError('settled envelope rose without key-on')
        observed_points = {str(offset):value for _,sample,value in active
                           if (offset:=sample-node['on_sample']) in POINTS}
        summaries.append({'voice':voice,'base_note':node['base_note'],
                          'gate_spc_cycles':off-node['on_cycle'],
                          'peak':peak,'peak_sample_offset':active[peak_index][1]-node['on_sample'],
                          'active_last':active[-1][2], 'active_points':observed_points,
                          'release_start':release[0], 'release_drops':len(drops),
                          'release_drop_samples':2,'release_zero':zero is not None,
                          'release_zero_spc_cycles':zero[0]-off if zero else None})
    return {'setup':edges[0]['voices'],'envelopes':summaries,'sample_count_per_voice':WINDOW}


# Exact published ENVX anchors from the owned fixture on both originals/voices.
ANCHORS = {2:(0,0,64,127,127,127,126,123,111,97,74),
           10:(0,0,2,6,18,38,82,125,113,99,79)}
LAST = {2:{'held':63,'short':84,'retrigger':110},
        10:{'held':70,'short':87,'retrigger':111}}
GATES = {'held':(334000,336000),'short':(203000,205000),'retrigger':(72000,74000)}
ENV_FIELDS = {'voice','base_note','gate_spc_cycles','peak','peak_sample_offset',
              'active_last','active_points','release_start','release_drops',
              'release_drop_samples','release_zero','release_zero_spc_cycles'}


def expected_points(instrument,case,note):
    count = 11 if case == 'held' else 10 if case == 'short' else 9
    points = dict(zip(map(str,POINTS[:count]),ANCHORS[instrument][:count]))
    if note == 25:
        points['8'] = 0
        if instrument == 10:
            points.update({'32':16,'128':80})
    return points


def contract(result,instrument,case):
    if type(instrument) is not int or instrument not in SETUPS or type(case) is not str or case not in CASES:
        raise ValueError('unknown envelope contract')
    expected_setup = [{'voice':voice,'pitch':PITCHES[instrument][0],
                       **dict(zip(('srcn','adsr1','adsr2','gain'),SETUPS[instrument]))}
                      for voice in (2,3)]
    if result.get('setup') != expected_setup or any(type(value) is not int
            for setup in result['setup'] for value in setup.values()) or type(
            result.get('sample_count_per_voice')) is not int or result['sample_count_per_voice'] != WINDOW:
        raise ValueError('envelope setup or sample-window contract differs')
    envelopes = result.get('envelopes')
    if not isinstance(envelopes,list) or len(envelopes) != 2*len(CASES[case][2]):
        raise ValueError('incomplete envelope note contract')
    for index,env in enumerate(envelopes):
        if not isinstance(env,dict) or set(env) != ENV_FIELDS or any(
                type(env[key]) is not int for key in ENV_FIELDS-{'active_points','release_zero'}):
            raise ValueError('invalid envelope summary fields or types')
        note = CASES[case][2][index//2]
        peak_offsets = (9,) if instrument == 2 and note == 24 else (10,) if instrument == 2 else (197,) if note == 24 else (198,199)
        last = LAST[instrument][case]
        points = expected_points(instrument,case,note)
        low,high = GATES[case]
        if (env['voice'] != 2+index%2 or env['base_note'] != note or env['peak'] != 127 or
                env['peak_sample_offset'] not in peak_offsets or env['active_last'] != last or
                env['active_points'] != points or not isinstance(env['active_points'],dict) or
                any(type(value) is not int for value in env['active_points'].values()) or
                env['release_start'] != last or env['release_drops'] != last or
                env['release_drop_samples'] != 2 or env['release_zero'] is not True or
                not 64*last <= env['release_zero_spc_cycles'] <= 64*last+128 or
                not low <= env['gate_spc_cycles'] <= high):
            raise ValueError('envelope measured trajectory contract differs')


def check_models(results):
    if not isinstance(results,list) or len(results) != 12 or any(
            not isinstance(result,dict) or type(result.get('instrument')) is not int or
            result.get('instrument') not in SETUPS or type(result.get('case')) is not str or result.get('case') not in CASES or
            result.get('model') not in ('sgb','sgb2') for result in results) or {
            (result['model'],result['instrument'],result['case']) for result in results} != {
            (model,instrument,case) for model in ('sgb','sgb2') for instrument in SETUPS for case in CASES}:
        raise ValueError('require all envelope fixtures/instruments on both models')
    for result in results:
        contract(result,result['instrument'],result['case'])
    for instrument in SETUPS:
        for case in CASES:
            pair = [result for result in results if result['instrument'] == instrument and result['case'] == case]
            for first,second in zip(pair[0]['envelopes'],pair[1]['envelopes']):
                if (abs(first['peak_sample_offset']-second['peak_sample_offset']) > 1 or
                        abs(first['gate_spc_cycles']-second['gate_spc_cycles']) > 1024 or
                        abs(first['release_zero_spc_cycles']-second['release_zero_spc_cycles']) > 128):
                    raise ValueError('two-model envelope trajectory differs')


def run(trace,firmware_dir,model,instrument,case):
    result = phrase_run(trace,firmware_dir,model,case,
                        fixture_builder=lambda name:build(instrument,name),
                        observer=lambda source:observe(source,instrument,case),
                        contract=lambda result,name:contract(result,instrument,name),
                        pattern_durations=None,trace_options=('--voice-envelope-trace',))
    result['instrument'] = instrument
    result['instruction_limit'] = 8000000
    return result


def dsp_window(source):
    """Fingerprint DSP writes only, for a sampler-on/off noninterference witness."""
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind','master_clock','spc_cycle','pcm_sample','address','value']:
        raise ValueError('incompatible DSP witness header')
    anchor,last_sample,previous,writes = None,0,-1,[]
    for count,row in enumerate(reader,1):
        if count >= MAX_ROWS:
            raise ValueError('DSP witness reached row bound')
        if row['kind'] not in ('D','E'):
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid DSP witness event')
        try:
            cycle,sample,address,value = (int(row[key]) for key in ('spc_cycle','pcm_sample','address','value'))
        except (TypeError,ValueError):
            raise ValueError('invalid DSP witness event') from None
        if cycle < previous or cycle < 0 or sample < 0 or not 0 <= address < 128 or not 0 <= value < 256:
            raise ValueError('invalid or unordered DSP witness event')
        previous = cycle
        last_sample = sample
        if anchor is not None and sample > anchor+WINDOW:
            break
        if row['kind'] != 'D':
            continue
        if address == 0x4C and value:
            if anchor is not None or value != 12:
                raise ValueError('DSP witness requires one combined onset')
            anchor = sample
        writes.append((cycle,sample,address,value))
    if anchor is None or last_sample < anchor+WINDOW:
        raise ValueError('incomplete DSP witness window')
    digest = hashlib.sha256(b''.join(struct.pack('<QQBB',*write) for write in writes)).hexdigest()
    return {'dsp_window_sha256':digest,'dsp_window_writes':len(writes)}


def noninterference(trace,firmware_dir):
    witnesses = []
    for model in ('sgb','sgb2'):
        pair = [phrase_run(trace,firmware_dir,model,'held',
                           fixture_builder=lambda case:build(10,case),observer=dsp_window,
                           contract=lambda result,case:None,pattern_durations=None,
                           trace_options=options)
                for options in ((),('--voice-envelope-trace',))]
        for field in ('dsp_window_sha256','dsp_window_writes'):
            if pair[0][field] != pair[1][field]:
                raise ValueError('ENVX sampler changed DSP write execution')
        witnesses.append({'model':model,'equal':True,
                          **{field:pair[0][field] for field in ('dsp_window_sha256','dsp_window_writes')}})
    return witnesses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(),args.firmware_dir.resolve(),model,instrument,case)
                   for instrument in (2,10) for case in CASES for model in ('sgb','sgb2')]
        check_models(results)
        witnesses = noninterference(args.trace.resolve(),args.firmware_dir.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-sgb-instrument-envelope-reference-v1',
                      'qualification':False,'playback':False,'runs':results,'sampler_noninterference':witnesses},indent=2))


if __name__ == '__main__':
    main()
