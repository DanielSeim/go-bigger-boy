#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored overflow/reload reads and HALT entry with an independent timer model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

SCENARIOS = tuple(f'{mode}-{modulo:02x}-phase-{phase}' for mode in ('poll', 'halt')
                  for modulo in (0, 226, 254) for phase in range(4))
INDEPENDENT_SCENARIOS = tuple(f'poll-{modulo:02x}-start-{start}-read-{read}'
    for modulo in (0, 226, 254) for start in range(4) for read in range(4))


def split(scenario):
    if scenario in SCENARIOS:
        mode, modulo, _, phase = scenario.split('-')
        return mode, int(modulo, 16), int(phase), int(phase)
    if scenario in INDEPENDENT_SCENARIOS:
        mode, modulo, _, start, _, read = scenario.split('-')
        return mode, int(modulo, 16), int(start), int(read)
    raise ValueError('unknown timer boundary scenario')


def read_times(mode, phase, read_phase=None):
    read_phase = phase if read_phase is None else read_phase
    enable = 20 + 4 * phase
    first_fall = (enable // 16 + 1) * 16
    first_read = (48 + 4 * phase + 4 * read_phase if mode == 'poll'
                  else first_fall + 4 + 20 + 16 + 4 * read_phase + 12)
    return [first_read + 80 * i for i in range(8)]


def expected_timer(mode, modulo, phase, read_phase=None):
    """CPU T-cycle oracle: bit-3 falls, four-cycle reload, IF latched until cleared.

    Register reads occur at the end of LDH's third machine cycle. HALT prediction
    assumes the next 4-T-cycle wake sample and 20-T-cycle interrupt entry.
    """
    enable = 20 + 4 * phase
    first_fall = (enable // 16 + 1) * 16
    reads = {time: (i, 0) for i, time in enumerate(read_times(mode, phase, read_phase))}
    reads.update({time + 20: (i, 1) for i, time in enumerate(read_times(mode, phase, read_phase))})
    result = [[0, 0] for _ in range(8)]
    counter, reload_delay, flags = 255, 0, 0
    for time in range(1, max(reads) + 1):
        if reload_delay:
            reload_delay -= 1
            if reload_delay == 0:
                counter, flags = modulo, flags | 4
        if time > enable and time % 16 == 0:
            if counter == 255:
                counter, reload_delay = 0, 4
            elif not reload_delay:
                counter += 1
        # Hardware acknowledges the initial timer request at entry in HALT mode.
        if mode == 'halt' and time == first_fall + 4 + 16:
            flags &= ~4
        if time in reads:
            index, field = reads[time]
            result[index][field] = counter if field == 0 else flags
    return result


def probe_rom(scenario, double_speed=False):
    mode, modulo, phase, read_phase = split(scenario)
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    begin = bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06')
    end = bytes.fromhex('3e 04 ea ff ff 3e 91 e0 40 3e 05 e0 07 fb 76 18 fd')
    start = rom.find(begin, 0x150, 0x300)
    if start < 0 or rom[start + len(begin):start + len(begin) + len(end)] != end:
        raise ValueError('isolated startup layout changed')
    code = bytearray.fromhex('3e a5 ea 00 c2 21 00 c1 af e0 41 e0 0f')
    code += bytes((0x3e, 4 if mode == 'halt' else 0)) + bytes.fromhex('ea ff ff')
    code += bytes((0x3e, modulo, 0xe0, 0x06)) + bytes.fromhex('3e ff e0 05 3e 91 e0 40 af e0 04')
    code += bytes(phase) + bytes.fromhex('3e 05 e0 07')
    code += bytes.fromhex('fb 00 76 18 fd' if mode == 'halt' else 'c3 00 03')
    if start + len(code) > 0x300:
        raise ValueError('boundary startup overlaps recorder')
    rom[start:start + len(code)] = code
    rom[0x50:0x53] = bytes.fromhex('c3 00 03')
    recorder = bytearray(read_phase)
    for _ in range(8):
        recorder += bytes.fromhex('f0 05 22 f0 0f 22 f0 44 22 f0 41 22')
    # The handler never returns; completion disables timer/IRQs and freezes WRAM.
    recorder += bytes.fromhex('af e0 07 ea ff ff 3e 08 ea 00 c0 3e 01 ea 01 c0 f3 76 18 fd')
    rom[0x300:0x380] = bytes(128)
    rom[0x300:0x300 + len(recorder)] = recorder
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    mode, modulo, phase, read_phase = split(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((8, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete timer boundary log or damaged guard')
    raw = [list(memory[i:i + 4]) for i in range(0x100, 0x120, 4)]
    timing = [[r[0], r[1] & 31] for r in raw]
    expected = expected_timer(mode, modulo, phase, read_phase)
    valid = all(r[2] < 144 and r[3] & 0x80 for r in raw)
    return dict(events=timing, observations=raw,
                event_fields=['tima', 'if_low5'], observation_fields=['tima', 'if', 'ly', 'stat'],
                start_phase_nops=phase, read_phase_nops=read_phase,
                predicted_tima_read_t_cycles=read_times(mode, phase, read_phase),
                prediction_assumptions=('end_of_bus_cycle; IME_off; IE_zero; no_IF_ack' if mode == 'poll'
                    else 'end_of_bus_cycle; next_4T_HALT_sample; 20T_IRQ_entry; IF_ack_at_entry_T16'),
                record_structure_match=bool(valid),
                expected_timer=expected, timer_oracle_match=timing == expected,
                authored_constraints_match=bool(valid and timing == expected),
                event_sha256=hashlib.sha256(bytes(v for r in timing for v in r)).hexdigest())


def run_probe(runner, adapter, output, independent_phases=False, case=None):
    scenarios = INDEPENDENT_SCENARIOS if independent_phases else SCENARIOS
    if case is not None:
        if case not in scenarios:
            raise ValueError('case does not belong to the selected phase matrix')
        scenarios = (case,)
    return isolated.run_probe(runner, adapter, output,
                              scenarios=scenarios,
                              checkpoints=(2,),
                              rom_builder=probe_rom, analyzer=analyze,
                              comparison_key='event_sha256', comparison_name='timer_boundary',
                              schema=('gbb-cgb-timer-independent-phase-probe-v1' if independent_phases
                                      else 'gbb-cgb-timer-boundary-probe-v1'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--independent-phases', action='store_true',
                        help='foreground DI matrix with separate timer-start and read delays')
    parser.add_argument('--case', help='run one case from the selected matrix')
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve(),
                           independent_phases=args.independent_phases, case=args.case)
        print(json.dumps(report))
        return 0 if isolated.passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'timer boundary probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
