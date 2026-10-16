#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded authored CGB STAT request and interrupt-service phase comparisons."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

SCENARIOS = ('timer-only', 'stat-only', 'mixed-short', 'mixed-pad', 'mixed-scroll', 'poll-stat')


def probe_rom(scenario, double_speed=False):
    if scenario not in SCENARIOS:
        raise ValueError('unknown IRQ timing scenario')
    poll = scenario == 'poll-stat'
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    begin = bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06')
    end = bytes.fromhex('3e 04 ea ff ff 3e 91 e0 40 3e 05 e0 07 fb 76 18 fd')
    start = rom.find(begin, 0x150, 0x300)
    if start < 0 or rom[start + len(begin):start + len(begin) + len(end)] != end:
        raise ValueError('isolated startup layout changed')
    ie = 0 if poll else (4 if scenario == 'timer-only' else 2 if scenario == 'stat-only' else 6)
    code = bytearray.fromhex('3e a5 ea 00 c2 06 20')  # Guard, persistent remaining-event count B.
    code += bytes((0x3e, 0 if scenario == 'timer-only' else 0x20, 0xe0, 0x41))
    code += begin + bytes((0x3e, ie))
    code += bytes.fromhex('ea ff ff 3e 91 e0 40 3e 05 e0 07')
    if poll:
        # IME stays off and IE=0. Observe the latched request; clear only STAT IF.
        code += bytes.fromhex('f0 0f e6 02 28 fa 3e 02 cd 40 03 f0 0f cb 8f e0 0f')
        code += bytes.fromhex('78 b7 20 eb 76 18 fd')
    else:
        code += bytes.fromhex('fb 76 18 fd')
    if start + len(code) > 0x300:
        raise ValueError('IRQ startup overlaps handlers')
    rom[start:start + len(code)] = code
    rom[0x48:0x4b] = bytes.fromhex('c3 00 03')
    rom[0x50:0x53] = bytes.fromhex('c3 20 03')
    # AF is saved per IRQ. B and HL are exclusively owned by the shared logger.
    stat = bytearray.fromhex('f5')
    if scenario in ('mixed-pad', 'mixed-scroll'):
        stat += bytes.fromhex('f0 44 e6 07')
        stat += bytes.fromhex('e0 43') if scenario == 'mixed-scroll' else bytes(3)
    stat += bytes.fromhex('3e 02 c3 40 03')
    rom[0x300:0x340] = bytes(64)  # Remove the isolated timer ISR before installing both paths.
    rom[0x300:0x300 + len(stat)] = stat
    rom[0x320:0x326] = bytes.fromhex('f5 3e 04 c3 40 03')
    # 32 tuples: source (STAT=2, timer=4), LY, IF & 31, TIMA.
    # Logging changes the workload; these are observation points, not exact assertion clocks.
    logger = bytearray.fromhex('22 f0 44 22 f0 0f e6 1f 22 f0 05 22 05 20 10')
    logger += bytes.fromhex('af e0 07 ea ff ff 3e 20 ea 00 c0 3e 01 ea 01 c0')
    logger += bytes.fromhex('c9' if poll else 'f1 d9')
    rom[0x340:0x340 + len(logger)] = logger
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    if len(memory) < 8192 or memory[:2] != b'\x20\x01' or memory[0x200] != 0xa5:
        raise ValueError('incomplete IRQ event log or damaged guard')
    events = [list(memory[i:i + 4]) for i in range(0x100, 0x180, 4)]
    sources = {e[0] for e in events}
    expected = {4} if scenario == 'timer-only' else {2} if scenario in ('stat-only', 'poll-stat') else {2, 4}
    valid = sources == expected and all(e[1] < 144 and e[2] < 32 for e in events)
    if scenario == 'poll-stat':
        valid = valid and all(e[2] & 2 for e in events)
    else:
        valid = valid and all(not e[2] & e[0] for e in events)  # Serviced IF was acknowledged.
    return dict(events=events, event_fields=['source', 'ly', 'if_low5', 'tima'],
                observation='polled_request' if scenario == 'poll-stat' else 'irq_handler',
                event_sha256=hashlib.sha256(memory[0x100:0x180]).hexdigest(),
                authored_constraints_match=bool(valid))


def run_probe(runner, adapter, output):
    return isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(2, 4),
                              rom_builder=probe_rom, analyzer=analyze,
                              comparison_key='event_sha256', comparison_name='irq_events',
                              schema='gbb-cgb-irq-timing-probe-v1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if isolated.passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'IRQ timing probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
