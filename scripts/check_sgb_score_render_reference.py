#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate owned native DSP rendering and bounded private note-onset timing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from build_sgb_score_render import build
from check_sgb_score_track_reference import align as align_track, validate as validate_track
from check_sgb_timing_reference import CASES, check_matrix, run

PITCHES = {0x98: 1068, 0x99: 1132, 0xA4: 2140}
TOLERANCE = 2048


def track_report(report):
    return {**report, 'schema': 'gbb-spc-score-track-v1'}


def validate(report, articulations=(127,)):
    if not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-render-v1':
        raise ValueError('invalid native renderer schema')
    validate_track(track_report(report))
    pcm = report.get('pcm')
    fields = ('frames', 'nonzero_frames', 'peak', 'fnv1a64', 'quiet_tail_frames')
    if not isinstance(pcm, dict) or any(type(pcm.get(key)) is not int for key in fields):
        raise ValueError('invalid owned PCM metadata')
    if (not 64 <= pcm['quiet_tail_frames'] <= pcm['frames'] <= 500000 or
            not 0 <= pcm['nonzero_frames'] <= pcm['frames'] or not 0 <= pcm['peak'] <= 32768 or
            not 0 <= pcm['fnv1a64'] < 2**64):
        raise ValueError('invalid owned PCM ranges')
    keyons = report.get('keyons')
    notes = [event for event in report['events'] if event['opcode'] != 0xC9]
    if not isinstance(keyons, list) or len(keyons) != len(notes):
        raise ValueError('native key-on count differs from scheduled notes')
    for index, keyon in enumerate(keyons):
        if not isinstance(keyon, dict) or any(type(keyon.get(key)) is not int for key in
                ('tick', 'opcode', 'pitch', 'half_cycle', 'quiet_tail_frames')):
            raise ValueError('invalid native key-on fields')
        note = notes[index]
        following = next((event['half_cycle'] for event in report['events'] if event['tick'] > note['tick']),
                         30_000_000)
        if (keyon['tick'] != note['tick'] or keyon['opcode'] != note['opcode'] or
                keyon['pitch'] != PITCHES.get(note['opcode']) or
                not note['half_cycle'] < keyon['half_cycle'] < following or
                not 0 <= keyon['quiet_tail_frames'] <= pcm['frames']):
            raise ValueError('native key-on pitch or timing differs from scheduled note')
    if any(event['articulation'] not in articulations or event['duration'] < 2 or
           event['opcode'] not in (*PITCHES, 0xC9) for event in report['events']):
        raise ValueError('renderer emitted events outside its supported profile')
    if notes:
        if pcm['nonzero_frames'] == 0 or pcm['peak'] == 0:
            raise ValueError('native notes produced no owned PCM')
    elif pcm['nonzero_frames'] != 0 or pcm['peak'] != 0:
        raise ValueError('silent or rejected stream produced PCM')
    return report


def native(probe, track, tempo):
    with tempfile.TemporaryDirectory(prefix='gbb-score-render-') as directory:
        program, stream = [Path(directory)/name for name in ('program.bin', 'track.bin')]
        program.write_bytes(build())
        stream.write_bytes(track)
        completed = subprocess.run([str(probe.resolve()), str(program), str(stream), str(tempo)],
                                   capture_output=True, text=True, timeout=60)
        if completed.returncode != 0 or len(completed.stdout) > 32768:
            raise ValueError('native renderer probe failed or exceeded report bound')
        return validate(json.loads(completed.stdout))


def align(report, track):
    validate(report)
    align_track(track_report(report), track)


def fixture(tempo=96):
    if tempo not in (96, 192):
        raise ValueError('unsupported renderer fixture tempo')
    # Independent warmup rest and final release rest; three measured pitches.
    return bytes((16, 127, 0xC9, 0x98, 0x99, 0xA4, 8, 0xC9, 0))


def compare(candidates, references):
    check_matrix(references)
    if set(candidates) != {96, 192}:
        raise ValueError('incomplete native renderer timing matrix')
    comparisons = []
    for result in references:
        report = candidates[result['tempo']]
        align(report, fixture(result['tempo']))
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(report['keyons'], report['keyons'][1:])]
        differences = [abs(a-b) for a, b in zip(intervals, result['onset_intervals_spc_cycles'])]
        if len(intervals) != 2 or any(value > TOLERANCE for value in differences):
            raise ValueError('native DSP onset differs beyond one timer pulse')
        comparisons.append({**result, 'native_DSP_onset_intervals_spc_cycles': intervals,
                            'absolute_difference_spc_cycles': differences})
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {tempo: native(args.probe, fixture(tempo), tempo) for tempo in (96, 192)}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                      for model in ('sgb', 'sgb2') for case in CASES]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'native renderer check failed: {error}\n')
    print(json.dumps({'schema': 'gbb-score-render-reference-v1', 'qualification': False,
                      'playback': False, 'evidence': 'native_DSP_onsets_and_owned_PCM',
                      'native_program_sha256': hashlib.sha256(build()).hexdigest(),
                      'tolerance_spc_cycles': TOLERANCE, 'owned_pcm': {
                          str(tempo): report['pcm'] for tempo, report in candidates.items()},
                      'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
