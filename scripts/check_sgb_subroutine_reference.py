#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe bounded finite subroutine playback in caller-owned original firmware."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from build_sgb_subroutine_fixture import build, CASES

ROOT = Path(__file__).resolve().parents[1]
MAX_ROWS = 32768
WATCHED = (0x22,0x23,0x24,0x3D)


def observe(source):
    reader=csv.DictReader(source)
    if reader.fieldnames != ['kind','master_clock','spc_cycle','pcm_sample','address','value']:
        raise ValueError('incompatible reference trace header')
    registers, pitches, cycles = {}, [], []
    previous_cycle=-1
    count=0
    for row in reader:
        count+=1
        if count>=MAX_ROWS:
            raise ValueError('reference trace reached its row bound')
        if row['kind']!='D':
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid reference DSP event')
        try:
            address,value,cycle=(int(row[key]) for key in ('address','value','spc_cycle'))
        except (TypeError,ValueError):
            raise ValueError('invalid reference DSP event') from None
        if not 0<=address<128 or not 0<=value<256 or cycle<0 or cycle<previous_cycle:
            raise ValueError('invalid or unordered reference DSP event')
        previous_cycle=cycle
        if address in WATCHED:
            registers[address]=value
        if address!=0x4C or value==0:
            continue
        if value!=4 or any(key not in registers for key in WATCHED):
            raise ValueError('unexpected voice or incomplete key-on setup')
        pitch=registers[0x22] | registers[0x23]<<8
        if registers[0x24]!=2 or registers[0x3D]&4 or pitch not in (1068,1132,2140):
            raise ValueError('unexpected reference source, noise or pitch')
        if cycles and cycle<=cycles[-1]:
            raise ValueError('unordered reference key-ons')
        pitches.append(pitch)
        cycles.append(cycle)
        if len(pitches)>8:
            raise ValueError('reference key-on count exceeds the fixture bound')
    if not pitches:
        raise ValueError('missing reference key-ons')
    return {'voice':2,'srcn':2,'pitches':pitches,
            'onset_intervals_spc_cycles':[b-a for a,b in zip(cycles,cycles[1:])]}


def check_contract(result,case):
    expected=[1068,1132]*CASES[case]+[2140,1068]
    if result['pitches']!=expected:
        raise ValueError('reference subroutine repeat/return sequence differs')
    intervals=result['onset_intervals_spc_cycles']
    if len(intervals)!=len(expected)-1 or any(not 84000<=value<=92000 for value in intervals):
        raise ValueError('reference subroutine onset timing differs')


def run(trace, firmware_directory, model, case):
    program = firmware_directory / ('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
    ipl = firmware_directory / 'spc700.rom'
    with tempfile.TemporaryDirectory(prefix='gbb-subroutine-reference-') as directory:
        base = Path(directory)
        game, boot, inputs, output = [base / name for name in ('subroutine.gb', 'boot.rom', 'none.script', 'dsp.csv')]
        image = build(case)
        game.write_bytes(image)
        header = (ROOT / f'firmware/gameboy/{model}_boot_image.hpp').read_text()
        boot_image = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', header))
        if len(boot_image) != 256:
            raise ValueError('invalid bundled original GB bootstrap')
        boot.write_bytes(boot_image)
        inputs.write_text('GBB SGB input v1\n0 none\n')
        command = [str(trace), str(program), str(ipl),
                   '--sync-gb-sgb1' if model == 'sgb' else '--sync-gb-sgb2', str(game), str(boot),
                   '--fractional-apu-sync', '--native-gb-input', '--input-script', str(inputs),
                   '--ppu-dma-timing', '--host-bus-timing', '--instruction-limit', '8000000',
                   '--sound-event-trace-output', str(output)]
        completed = subprocess.run(command, capture_output=True, timeout=180)
        if completed.returncode != 4 or not output.exists():
            raise ValueError(f'{model}: reference did not finish at the instruction bound')
        if output.stat().st_size > 16 * 1024 * 1024:
            raise ValueError('reference DSP trace exceeds the byte bound')
        with output.open() as source:
            result = observe(source)
        check_contract(result, case)
        result['call_count'] = CASES[case]
        return {'model': model, 'case': case, **result,
                'fixture_sha256': hashlib.sha256(image).hexdigest(),
                'gb_boot_sha256': hashlib.sha256(boot_image).hexdigest(),
                'program_sha256': hashlib.sha256(program.read_bytes()).hexdigest(),
                'ipl_sha256': hashlib.sha256(ipl.read_bytes()).hexdigest()}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        results=[run(args.trace.resolve(),args.firmware_dir.resolve(),model,case)
                 for case in CASES for model in ('sgb','sgb2')]
        for index in range(0,len(results),2):
            first,second=results[index:index+2]
            if first['pitches']!=second['pitches'] or any(abs(a-b)>2048 for a,b in zip(
                    first['onset_intervals_spc_cycles'],second['onset_intervals_spc_cycles'])):
                raise ValueError('two-model reference subroutine playback differs')
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-sgb-subroutine-reference-v1','qualification':False,
                      'playback':False,'evidence':'original_program_DSP_writes_with_owned_subroutine_fixture',
                      'runs':results},indent=2))


if __name__=='__main__':
    main()
