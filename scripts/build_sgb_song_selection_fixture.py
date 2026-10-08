#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build original homebrew that uploads three empty songs and requests their IDs."""
import argparse
import hashlib
from pathlib import Path
import struct


def score_payload():
    bank = bytearray(32)
    struct.pack_into('<HHH', bank, 0, 0x2B10, 0x2B14, 0x2B18)
    # Each distinct phrase root immediately ends. No vendor samples, notes,
    # instruments, quantization tables or program bytes are needed by this bank.
    data = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return data + bytes(4096 - len(data))


def build(order=(1, 2, 3)):
    return build_cartridge(score_payload(), order)


def build_cartridge(payload, order, *, wait_frames=16, repeat_upload=False, sound_fields=(0, 0, 0), commands=None):
    if not isinstance(payload, bytes) or len(payload) != 4096:
        raise ValueError('fixture transfer must contain exactly 4096 bytes')
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 8:
        raise ValueError('order requires 1..8 song IDs')
    if any(type(code) is not int or code not in (1, 2, 3) for code in order):
        raise ValueError('song IDs must be integers 1, 2 or 3')
    if type(wait_frames) is not int or not 16 <= wait_frames <= 128 or type(repeat_upload) is not bool:
        raise ValueError('invalid bounded transfer spacing')
    if not isinstance(sound_fields, (tuple, list)) or len(sound_fields) != 3 or any(
            type(value) is not int or not 0 <= value <= 255 for value in sound_fields):
        raise ValueError('invalid SOUND effect/attribute fields')
    if commands is not None:
        if not isinstance(commands, (tuple, list)) or not 1 <= len(commands) <= 8:
            raise ValueError('commands require 1..8 bounded operations')
        for command in commands:
            if not isinstance(command, (tuple, list)) or len(command) != 2:
                raise ValueError('command requires frame delay and packet')
            delay, values = command
            if type(delay) is not int or not 1 <= delay <= 128 or not isinstance(values, bytes):
                raise ValueError('invalid command spacing/packet')
            if not 1 <= len(values) <= 16 or values[0] not in (0x41, 0x49):
                raise ValueError('only bounded SOUND/SOU_TRN commands are admitted')
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
    rom[0x4000:0x5000] = payload
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
    if commands is not None:
        for delay, values in commands:
            frames(delay)
            packet(values)
    else:
        for index, song in enumerate(order):
            if repeat_upload and index:
                frames(wait_frames)
                packet([0x49])
            frames(wait_frames)
            packet([0x41, *sound_fields, song])
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--order', default='1,2,3', help='Comma-separated song IDs, each 1..3')
    args = parser.parse_args()
    try:
        image = build([int(value) for value in args.order.split(',')])
        with args.output.open('xb') as out:
            out.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Original song-selection fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
