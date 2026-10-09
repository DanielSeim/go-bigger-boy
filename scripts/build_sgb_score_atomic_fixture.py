#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned physical replacement uploads, including incomplete transaction lists."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_relocated_fixture import assets, bank

VALID = ('full', 'split', 'reverse', 'interleave')
FAULTS = ('missing-score', 'missing-assets', 'empty', 'score-head', 'score-tail',
          'score-gap', 'score-overlap', 'asset-head', 'asset-tail', 'asset-gap',
          'asset-overlap', 'backward', 'code', 'cache', 'asset-overflow', 'entry',
          'framing', 'bad-content')


def payload(kind='full', *, replacement=False):
    if kind not in VALID + FAULTS or type(replacement) is not bool:
        raise ValueError('unknown transaction or invalid replacement flag')
    score = bank('inherit' if replacement else 'all2')
    asset = assets('mixed2' if replacement else 'mixed3',
                   profile='distinct' if replacement else 'baseline',
                   brr_profile='swap' if replacement else 'mixed',
                   layout='last' if replacement else 'near')
    chunks = [(0x2B00, score), (0x5000, asset)]
    entry = 0x0400
    if kind == 'split':
        chunks = [(0x2B00, score[:1]), (0x2B01, score[1:257]), (0x2C01, score[257:]),
                  (0x5000, asset[:19]), (0x5013, asset[19:64]), (0x5040, asset[64:])]
    elif kind == 'reverse':
        chunks.reverse()
    elif kind == 'interleave':
        chunks = [(0x2B00, score[:256]), (0x5000, asset[:20]),
                  (0x2C00, score[256:]), (0x5014, asset[20:])]
    elif kind == 'missing-score':
        chunks = chunks[1:]
    elif kind == 'missing-assets':
        chunks = chunks[:1]
    elif kind == 'empty':
        chunks = []
    elif kind == 'score-head':
        chunks[0] = (0x2B00, score[:6])
    elif kind == 'score-tail':
        chunks[0] = (0x2B00, score[:-1])
    elif kind == 'score-gap':
        chunks = [(0x2B00, score[:256]), (0x2C01, score[257:]), chunks[1]]
    elif kind == 'score-overlap':
        chunks = [chunks[0], (0x2B00, score[:1]), chunks[1]]
    elif kind == 'asset-head':
        chunks[1] = (0x5000, asset[:64])
    elif kind == 'asset-tail':
        chunks[1] = (0x5000, asset[:-1])
    elif kind == 'asset-gap':
        chunks = [chunks[0], (0x5000, asset[:20]), (0x5015, asset[21:])]
    elif kind == 'asset-overlap':
        chunks += [(0x5000, asset[:1])]
    elif kind == 'backward':
        chunks = [(0x2C00, score[256:]), (0x2B00, score[:256]), chunks[1]]
    elif kind in ('code', 'cache'):
        chunks += [(0x1800 if kind == 'code' else 0x6000, bytes(1))]
    elif kind == 'asset-overflow':
        chunks[1] = (0x5000, asset+bytes(1))
    elif kind == 'entry':
        entry = 0x0401
    elif kind == 'framing':
        return struct.pack('<HH', 4096, 0x2B00) + bytes(4092)
    elif kind == 'bad-content':
        changed = bytearray(asset)
        changed[0] = 4
        chunks[1] = (0x5000, bytes(changed))
    data = b''.join(struct.pack('<HH', len(data), address)+data for address, data in chunks)
    data += struct.pack('<HH', 0, entry)
    if len(data) > 4096:
        raise ValueError('transaction exceeds physical frame')
    return data + bytes(4096-len(data))


def build_cartridge(payloads, commands, *, io_writes=None):
    if not isinstance(payloads, (tuple, list)) or not 1 <= len(payloads) <= 3 or any(
            not isinstance(p, bytes) or len(p) != 4096 for p in payloads):
        raise ValueError('requires 1..3 physical payloads')
    if not isinstance(commands, (tuple, list)) or not 1 <= len(commands) <= 8:
        raise ValueError('requires 1..8 commands')
    for command in commands:
        if not isinstance(command, (tuple, list)) or len(command) != 3:
            raise ValueError('command requires delay, packet and optional payload index')
        delay, values, index = command
        if type(delay) is not int or not 1 <= delay <= 128 or (values is not None and (not isinstance(values, bytes) or not 1 <= len(values) <= 16 or values[0] not in (0x41, 0x49))):
            raise ValueError('invalid command')
        if index is not None and (type(index) is not int or not 0 <= index < len(payloads)):
            raise ValueError('invalid payload index')
    if io_writes is None:
        io_writes = [()] * len(commands)
    if not isinstance(io_writes,(tuple,list)) or len(io_writes)!=len(commands) or any(
            not isinstance(writes,(tuple,list)) or len(writes)>12 or any(
                not isinstance(pair,(tuple,list)) or len(pair)!=2 or
                type(pair[0]) is not int or pair[0] not in (*range(0x10,0x27),0x80) or
                type(pair[1]) is not int or not 0<=pair[1]<=255 for pair in writes) for writes in io_writes):
        raise ValueError('invalid bounded APU/HRAM writes')
    if any(values is None and (index is not None or not writes) for (_,values,index),writes in zip(commands,io_writes)):
        raise ValueError('IO-only action requires writes and no upload')
    rom = bytearray(32768)
    rom[0x100:0x103] = bytes.fromhex('c35001')
    # Fixed cartridge-header verification signature, already used by
    # src/cartridge.cpp and the mapper fixtures; never copied from a ROM.
    rom[0x104:0x134] = bytes.fromhex(
        'ceed6666cc0d000b03730083000c000d0008111f8889000e'
        'dccc6ee6ddddd999bbbb67636e0eecccdddc999fbbb9333e')
    rom[0x134:0x13E] = b'GBB SELECT'
    rom[0x146] = 3
    rom[0x14B] = 0x33
    for index, payload in enumerate(payloads):
        rom[0x4000+4096*index:0x5000+4096*index] = payload
    code = bytearray.fromhex('f3afe0401100402100800100101a22130b78b120f8')
    for row in range(13):
        address = 0x9800 + row * 32
        code.extend((0x21, address & 255, address >> 8))
        for x in range(20):
            tile = row * 20 + x
            if tile < 256:
                code.extend((0x3E, tile, 0x22))
    code.extend(bytes.fromhex('3ee4e0473e91e040'))

    def frames(count):
        for _ in range(count):
            # Wait through the end of VBlank and into the next VBlank.
            code.extend(bytes.fromhex('f044fe9030faf044fe9038fa'))

    def packet(values):
        def joy(value):
            code.extend((0x3E, value, 0xE0, 0))
        joy(0x30); joy(0); joy(0x30)
        for value in bytes(values) + bytes(16 - len(values)):
            for bit in range(8):
                joy(0x10 if value & (1 << bit) else 0x20)
                joy(0x30)
        joy(0x20); joy(0x30)

    packet([0x49])
    for command_index,(delay, values, index) in enumerate(commands):
        frames(delay)
        if index is not None:
            # LCD is in VBlank after frames(); upload a different owned payload
            # through actual GB VRAM with LCD disabled, then restart the display.
            code.extend(bytes.fromhex('afe040'))
            source = 0x4000 + 4096*index
            code.extend((0x11, source & 255, source >> 8))
            code.extend(bytes.fromhex('2100800100101a22130b78b120f8'))
            code.extend(bytes.fromhex('3e91e040'))
            frames(2)
        for address,value in io_writes[command_index]:
            code.extend((0x3E,value,0xE0,address))
        if values is not None:
            packet(values)
    code.extend(bytes.fromhex('18fe'))
    if 0x150 + len(code) > 0x4000:
        raise ValueError('fixture code overlaps transfer data')
    rom[0x150:0x150 + len(code)] = code
    # Valid header/global checksums let native program firmware accept the
    # bootstrap's header packets as an ordinary SGB-capable cartridge.
    checksum = 0
    for value in rom[0x134:0x14D]:
        checksum = (checksum - value - 1) & 255
    rom[0x14D] = checksum
    struct.pack_into('>H', rom, 0x14E, sum(rom) & 65535)
    return bytes(rom)


def build(kind='full', *, active=False, stop=False, cold=False, third=False):
    if any(type(flag) is not bool for flag in (active, stop, cold, third)):
        raise ValueError('fixture flags must be boolean')
    if cold and (active or stop or third):
        raise ValueError('cold fixture cannot include lifecycle spacing')
    replacement = payload(kind, replacement=True)
    if cold:
        return build_cartridge((replacement,), ((64, bytes((0x41,0,0,0,1)), None),))
    payloads = [payload(), replacement]
    commands = [(64, bytes((0x41,0,0,0,1)), None)]
    if stop:
        commands += [(6 if active else 64, bytes((0x41,0,0,0,128)), None)]
    commands += [(4 if active else 64, bytes((0x49,)), 1),
                 (64, bytes((0x41,0,0,0,3)), None)]
    if third:
        payloads += [payload('interleave')]
        commands += [(64, bytes((0x49,)), 2), (64, bytes((0x41,0,0,0,2)), None)]
    return build_cartridge(payloads, commands)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kind', choices=VALID+FAULTS, default='full')
    parser.add_argument('--active', action='store_true')
    parser.add_argument('--stop', action='store_true')
    parser.add_argument('--cold', action='store_true')
    parser.add_argument('--third', action='store_true')
    args = parser.parse_args()
    try:
        image = build(args.kind, active=args.active, stop=args.stop, cold=args.cold, third=args.third)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
