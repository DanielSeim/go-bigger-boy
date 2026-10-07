#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure bounded note onset/gate timing from caller-owned original SGB firmware."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from build_sgb_timing_fixture import build, CASES, NOTES, DURATION

ROOT = Path(__file__).resolve().parents[1]
MAX_ROWS = 32768
# Compare observations within one reference, then across models. These ranges
# allow native scheduler phase jitter; they do not define a general tempo law.
MODEL_TOLERANCE = 2048


def observe(source):
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible reference trace header')
    registers, notes = {}, []
    previous_cycle = -1
    count = 0
    for row in reader:
        count += 1
        if count >= MAX_ROWS:
            raise ValueError('reference trace reached its row bound')
        if row['kind'] != 'D':
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid reference DSP event')
        try:
            address, value, cycle = (int(row[key]) for key in ('address', 'value', 'spc_cycle'))
        except (TypeError, ValueError):
            raise ValueError('invalid reference DSP event') from None
        if not 0 <= address < 128 or not 0 <= value < 256 or cycle < 0 or cycle < previous_cycle:
            raise ValueError('invalid or unordered reference DSP event')
        previous_cycle = cycle
        if address in (0x22, 0x23, 0x24, 0x3D):
            registers[address] = value
        if address == 0x5C and value & 4 and notes and 'off' not in notes[-1]:
            if cycle <= notes[-1]['on']:
                raise ValueError('nonpositive reference note gate')
            notes[-1]['off'] = cycle
        if address != 0x4C or value == 0:
            continue
        if value != 4 or any(key not in registers for key in (0x22, 0x23, 0x24, 0x3D)):
            raise ValueError('unexpected voice or incomplete key-on setup')
        if registers[0x24] != 2 or registers[0x3D] & 4:
            raise ValueError('unexpected reference source or noise voice')
        if notes and 'off' not in notes[-1]:
            raise ValueError('note retriggered without a reference key-off')
        notes.append({'on': cycle, 'pitch': registers[0x22] | registers[0x23] << 8})
    if len(notes) != 3 or any('off' not in note for note in notes):
        raise ValueError('expected three complete reference note gates')
    if [note['pitch'] for note in notes] != [1068, 1132, 2140]:
        raise ValueError('reference note pitches differ from the fixture contract')
    intervals = [notes[i+1]['on'] - notes[i]['on'] for i in (0, 1)]
    if any(interval <= 0 for interval in intervals):
        raise ValueError('nonpositive reference onset interval')
    gates = [note['off'] - note['on'] for note in notes]
    if any(gates[i] >= intervals[i] for i in (0, 1)):
        raise ValueError('reference gate overlaps the next onset')
    return {'voice': 2, 'base_notes': list(NOTES), 'onset_intervals_spc_cycles': intervals,
            'gate_spc_cycles': gates}


def check_matrix(results):
    if len(results) != 6 or {(r['model'], r['case']) for r in results} != {
            (model, case) for model in ('sgb', 'sgb2') for case in CASES}:
        raise ValueError('incomplete two-model timing matrix')
    gate_bounds = {'baseline': (72000, 76000), 'double-tempo': (34000, 39000),
                   'short-gate': (44000, 49000)}
    for result in results:
        for field, size in (('onset_intervals_spc_cycles', 2), ('gate_spc_cycles', 3)):
            values = result[field]
            if len(values) != size or any(type(value) is not int or value <= 0 for value in values):
                raise ValueError('invalid timing matrix intervals')
        low, high = gate_bounds[result['case']]
        if any(not low <= value <= high for value in result['gate_spc_cycles']):
            raise ValueError('note gate timing is outside the observed range')
    summaries = []
    for model in ('sgb', 'sgb2'):
        cases = {r['case']: r for r in results if r['model'] == model}
        baseline, faster, shorter = [cases[c] for c in CASES]
        intervals = baseline['onset_intervals_spc_cycles']
        # Baseline duration-16 events were observed near 87k SPC cycles.
        if any(not 84000 <= interval <= 90000 for interval in intervals):
            raise ValueError('baseline onset timing is outside the observed range')
        tempo_ratios = [fast/base for fast, base in zip(faster['onset_intervals_spc_cycles'], intervals)]
        short_onset_ratios = [short/base for short, base in zip(shorter['onset_intervals_spc_cycles'], intervals)]
        gate_ratios = [short/base for short, base in zip(shorter['gate_spc_cycles'], baseline['gate_spc_cycles'])]
        if any(not 0.47 <= ratio <= 0.53 for ratio in tempo_ratios):
            raise ValueError('double-tempo onset interval contract failed')
        if any(not 0.97 <= ratio <= 1.03 for ratio in short_onset_ratios):
            raise ValueError('short articulation changed onset spacing')
        if any(not 0.59 <= ratio <= 0.67 for ratio in gate_ratios):
            raise ValueError('short articulation did not shorten note gates')
        summaries.append({'model': model, 'double_tempo_interval_ratios': tempo_ratios,
                          'short_gate_onset_ratios': short_onset_ratios,
                          'short_gate_duration_ratios': gate_ratios})
    for case in CASES:
        first, second = [next(r for r in results if r['case'] == case and r['model'] == model)
                         for model in ('sgb', 'sgb2')]
        for field in ('onset_intervals_spc_cycles', 'gate_spc_cycles'):
            if any(abs(a-b) > MODEL_TOLERANCE for a, b in zip(first[field], second[field])):
                raise ValueError('two-model reference timing differs beyond 2048 SPC cycles')
    return summaries


def run(trace, firmware_directory, model, case):
    tempo, articulation = CASES[case]
    return run_fixture(trace, firmware_directory, model, case, build(case), tempo, articulation, DURATION)


def run_fixture(trace, firmware_directory, model, case, image, tempo, articulation, duration):
    if model not in ('sgb', 'sgb2'):
        raise ValueError('unknown timing reference model')
    program = firmware_directory / ('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
    ipl = firmware_directory / 'spc700.rom'
    with tempfile.TemporaryDirectory(prefix='gbb-timing-reference-') as directory:
        base = Path(directory)
        game, boot, inputs, output = [base / name for name in ('timing.gb', 'boot.rom', 'none.script', 'dsp.csv')]
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
        return {'model': model, 'case': case, 'tempo': tempo, 'articulation': articulation,
                'duration': duration, **result,
                'fixture_sha256': hashlib.sha256(image).hexdigest(),
                'gb_boot_sha256': hashlib.sha256(boot_image).hexdigest(),
                'program_sha256': hashlib.sha256(program.read_bytes()).hexdigest(),
                'ipl_sha256': hashlib.sha256(ipl.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                   for case in CASES for model in ('sgb', 'sgb2')]
        comparisons = check_matrix(results)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-timing-reference-v1', 'qualification': False,
                      'playback': False, 'evidence': 'original_program_DSP_writes_with_owned_note_fixture',
                      'runs': results, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
