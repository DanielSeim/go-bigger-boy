#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned boundary overwrite with explicit and inherited next-pattern timing."""
import struct
from build_sgb_score_order_fixture import bank as prior_bank,expected as prior_expected,CASES as PRIOR_CASES
from build_sgb_song_selection_fixture import build_cartridge
CASES=(*PRIOR_CASES,'inherit-note','inherit-rest')


def bank(articulation=127,case='end-3-note'):
    if case not in CASES:raise ValueError('invalid reverse fixture')
    data=bytearray(prior_bank(articulation,'end-3-'+case.split('-')[-1] if case.startswith('inherit') else case))
    if case.startswith('inherit'):
        # No timing bytes: retain the duration/articulation of the event that
        # executes before the channel-3 end, despite never getting a KON.
        data[0x6FD:0x700]=bytes((0x9A,0x9B,0))
    return bytes(data)


def expected(case):
    if case not in CASES:raise ValueError('invalid reverse fixture')
    return prior_expected('end-3-'+case.split('-')[-1] if case.startswith('inherit') else case)


def ticks(case):
    if case not in CASES:raise ValueError('invalid reverse fixture')
    return (0,16,32,40,48,64,80,96) if case.startswith('inherit') else tuple(range(0,128,16))


def build(articulation=127,case='end-3-note'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
