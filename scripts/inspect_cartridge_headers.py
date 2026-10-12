#!/usr/bin/env python3
"""Read-only, offline cartridge metadata checks. Never imports network clients.

No game-title or ROM-hash mapper overrides are used. --expected-type lets the
operator compare headers with independently known cartridge/board metadata.
"""
import argparse
import json
from pathlib import Path
import sys

LOGO = bytes.fromhex('ceed6666cc0d000b03730083000c000d0008111f8889000edccc6ee6ddddd999'
                     'bbbb67636e0eecccdddc999fbbb9333e')
TYPES = {0x00:'ROM', 0x01:'MBC1', 0x02:'MBC1+RAM', 0x03:'MBC1+RAM+BATTERY',
         0x05:'MBC2', 0x06:'MBC2+BATTERY', 0x08:'ROM+RAM', 0x09:'ROM+RAM+BATTERY',
         0x0B:'MMM01', 0x0C:'MMM01+RAM', 0x0D:'MMM01+RAM+BATTERY',
         0x0F:'MBC3+RTC', 0x10:'MBC3+RTC+RAM', 0x11:'MBC3', 0x12:'MBC3+RAM',
         0x13:'MBC3+RAM+BATTERY', 0x19:'MBC5', 0x1A:'MBC5+RAM',
         0x1B:'MBC5+RAM+BATTERY', 0x1C:'MBC5+RUMBLE', 0x1D:'MBC5+RUMBLE+RAM',
         0x1E:'MBC5+RUMBLE+RAM+BATTERY', 0x22:'MBC7', 0xFC:'CAMERA',
         0xFD:'TAMA5', 0xFE:'HuC3', 0xFF:'HuC1'}
RAM = {0:0,1:2048,2:8192,3:32768,4:131072,5:65536}


def inspect(path, expected_type=None):
    result = {'path':str(path), 'errors':[], 'warnings':[], 'game_identity_verified':False}
    errors, warnings = result['errors'], result['warnings']
    with path.open('rb') as stream:
        size = stream.seek(0, 2)
        result['file_bytes'] = size
        stream.seek(0)
        first = stream.read(0x150)
        if len(first) < 0x150:
            errors.append('file_too_short_for_header')
            return result
        offset = 0
        header = first
        layout = 'ordinary'
        if size >= 32768:
            menu_offset = size - 32768
            stream.seek(menu_offset)
            menu = stream.read(0x150)
            if menu[0x147] in (0x0B,0x0C,0x0D) and menu[0x104:0x134] == LOGO:
                offset, header = menu_offset, menu
                layout = 'physical_menu_last'
                if first[0x147] != menu[0x147]:
                    warnings.append('MMM01_menu_header_differs_from_contained_game_header')
            elif first[0x147] in (0x0B,0x0C,0x0D):
                checksum = (-sum(first[0x134:0x14D]) - 25) & 255
                code=first[0x148]
                declared=32768 << code if code<=8 else {0x52:72*16384,0x53:80*16384,0x54:96*16384}.get(code)
                if size>32768 and first[0x104:0x134]==LOGO and checksum==first[0x14D] and declared==size:
                    layout='menu_first_normalized_on_load'
                elif size==32768 and first[0x104:0x134]==LOGO:
                    layout='physical_menu_last'
                else:
                    layout='unverified_MMM01'
                    warnings.append('MMM01_power_on_menu_header_not_verified')
        kind, color, rom_code, ram_code = (header[i] for i in (0x147,0x143,0x148,0x149))
        declared_rom = 32768 << rom_code if rom_code <= 8 else {0x52:72*16384,0x53:80*16384,0x54:96*16384}.get(rom_code)
        declared_ram = RAM.get(ram_code)
        checksum = (-sum(header[0x134:0x14D]) - 25) & 255
        stream.seek(0)
        total = 0
        while chunk := stream.read(65536):
            total = (total + sum(chunk)) & 65535
        global_checksum = (total - header[0x14E] - header[0x14F]) & 65535
        result.update(header_offset=offset, dump_layout=layout,
                      normalized_header_offset=size-32768 if layout=='menu_first_normalized_on_load' else offset, title=header[0x134:0x143 if color & 128 else 0x144].split(b'\0')[0].decode('ascii','replace'),
                      controller_type=f'0x{kind:02X}', controller_name=TYPES.get(kind,'unsupported'),
                      cgb_flag=f'0x{color:02X}', cgb_mode='required' if color & 192 == 192 else 'supported' if color & 128 else 'DMG',
                      rom_size_code=f'0x{rom_code:02X}', declared_rom_bytes=declared_rom,
                      ram_size_code=f'0x{ram_code:02X}', declared_ram_bytes=declared_ram,
                      header_checksum_valid=checksum == header[0x14D], logo_valid=header[0x104:0x134] == LOGO,
                      global_checksum_valid=global_checksum == int.from_bytes(header[0x14E:0x150],'big'))
        if declared_rom is None:
            errors.append('unknown_ROM_size_code')
        elif size != declared_rom:
            errors.append('file_size_differs_from_declared_ROM_size')
        if not result['header_checksum_valid']:
            errors.append('invalid_header_checksum')
        if not result['logo_valid']:
            warnings.append('invalid_boot_logo')
        if not result['global_checksum_valid']:
            warnings.append('global_checksum_mismatch_not_a_mapper_override')
        if color & 128 and color not in (0x80,0xC0):
            warnings.append('noncanonical_CGB_flag')
        fixed = {0x05:512,0x06:512,0x22:256,0xFD:32,0xFC:131072}
        result['effective_nonvolatile_bytes'] = fixed.get(kind, declared_ram)
        if kind in fixed:
            if ram_code != 0 and kind in (0x22,0xFD):
                warnings.append('generic_RAM_header_does_not_describe_internal_EEPROM_or_TAMA_memory')
        elif declared_ram is None:
            errors.append('unknown_RAM_size_code')
        if kind not in TYPES:
            errors.append('controller_not_supported')
        if kind == 0x0B and ram_code != 0:
            errors.append('ROM_only_MMM01_type_advertises_RAM')
        rom_limits = {0x0B:8388608,0x0C:8388608,0x0D:8388608,0x22:2097152,0xFD:524288,0xFE:2097152,0xFF:1048576}
        if kind in rom_limits and declared_rom is not None and declared_rom > rom_limits[kind]:
            errors.append('ROM_exceeds_controller_address_limit')
        if kind == 0xFF and declared_ram is not None and declared_ram > 32768:
            errors.append('RAM_exceeds_HuC1_address_limit')
        if expected_type is not None:
            result['expected_controller_type'] = f'0x{expected_type:02X}'
            result['controller_matches_external_metadata'] = kind == expected_type
            if kind != expected_type:
                errors.append('controller_differs_from_external_metadata')
    return result


def byte_value(text):
    value = int(text, 0)
    if not 0 <= value <= 255:
        raise argparse.ArgumentTypeError('expected a byte from 0 to 255')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    parser.add_argument('--expected-type', type=byte_value)
    parser.add_argument('--recursive', action='store_true', help='inspect .gb/.gbc files within directories')
    args = parser.parse_args()
    files = []
    for path in args.paths:
        if path.is_dir() and args.recursive:
            files.extend(sorted(p for p in path.rglob('*') if p.is_file() and p.suffix.lower() in ('.gb','.gbc')))
        else:
            files.append(path)
    if not files:
        print(json.dumps({'errors':['no_ROM_files_found']}))
        return 1
    failed = False
    for path in files:
        try:
            result = inspect(path,args.expected_type)
        except OSError as error:
            result = {'path':str(path),'errors':[str(error)],'warnings':[]}
        print(json.dumps(result,ensure_ascii=True,sort_keys=True))
        failed |= bool(result['errors'])
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
