#!/usr/bin/env python3
"""Owned, synthetic fixtures for the offline Synology metadata tool."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/inspect_cartridge_headers.py'
spec = importlib.util.spec_from_file_location('headers', SCRIPT)
headers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(headers)


def rom(kind, size=32768, ram=0):
    data = bytearray(size)
    data[0x104:0x134] = headers.LOGO
    data[0x134:0x138] = b'TEST'
    data[0x147] = kind
    data[0x148] = (size // 32768).bit_length() - 1
    data[0x149] = ram
    data[0x14D] = (-sum(data[0x134:0x14D]) - 25) & 255
    data[0x14E:0x150] = (sum(data) & 65535).to_bytes(2,'big')
    return data


class HeaderTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)/'fixture.gb'

    def inspect(self, data, expected=None):
        self.path.write_bytes(data)
        before = self.path.read_bytes()
        result = headers.inspect(self.path,expected)
        self.assertEqual(self.path.read_bytes(),before)
        return result

    def test_rare_types_and_internal_memory(self):
        for kind in (0x0B,0x22,0xFD,0xFE,0xFF):
            with self.subTest(kind=kind):
                result = self.inspect(rom(kind),kind)
                self.assertEqual(result['errors'],[])
                self.assertTrue(result['controller_matches_external_metadata'])
                self.assertFalse(result['game_identity_verified'])
                self.assertTrue(result['header_checksum_valid'])
                self.assertTrue(result['global_checksum_valid'])
        self.assertEqual(self.inspect(rom(0x22))['effective_nonvolatile_bytes'],256)
        self.assertEqual(self.inspect(rom(0xFD))['effective_nonvolatile_bytes'],32)

    def test_broken_headers_are_reported(self):
        data=rom(0xFF); data[0x148]=8; data[0x149]=0xFF
        result=self.inspect(data,0x22)
        for error in ('file_size_differs_from_declared_ROM_size','invalid_header_checksum',
                      'unknown_RAM_size_code','controller_differs_from_external_metadata'):
            self.assertIn(error,result['errors'])
        self.assertIn('file_too_short_for_header',self.inspect(b'bad')['errors'])
        self.assertIn('controller_not_supported',self.inspect(rom(0x20))['errors'])

    def test_mmm01_menu_header(self):
        data=rom(1,131072)
        menu=rom(0x0D,131072,3)[:0x150]
        data[-32768:-32768+0x150]=menu
        result=self.inspect(data)
        self.assertEqual(result['controller_type'],'0x0D')
        self.assertEqual(result['header_offset'],98304)
        self.assertIn('MMM01_menu_header_differs_from_contained_game_header',result['warnings'])

    def test_cli_json_exit_codes_and_no_files_written(self):
        self.path.write_bytes(rom(0xFD))
        before=set(self.path.parent.iterdir())
        result=subprocess.run([sys.executable,str(SCRIPT),str(self.path),'--expected-type','0xfd'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(result.stdout)['controller_name'],'TAMA5')
        self.assertEqual(set(self.path.parent.iterdir()),before)
        bad=subprocess.run([sys.executable,str(SCRIPT),str(self.path),'--expected-type','0xff'],capture_output=True,text=True)
        self.assertEqual(bad.returncode,1)
        self.assertIn('controller_differs_from_external_metadata',json.loads(bad.stdout)['errors'])


if __name__ == '__main__':
    unittest.main()
