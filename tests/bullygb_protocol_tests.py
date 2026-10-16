#!/usr/bin/env python3
"""Authored BullyGB terminal-screen protocol regressions; no upstream assets."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = str(Path(sys.argv.pop(1)).resolve())


def rom(text, *, lcd=True, terminal=True):
    data = bytearray(32768)
    data[0x100:0x103] = bytes.fromhex('c3 50 01')
    code = bytearray.fromhex('f3 af e0 40')
    for i, char in enumerate(text.encode('ascii')):
        address = 0x9821 + (i // 18) * 32 + i % 18
        code += bytes((0x3e, char, 0xea, address & 255, address >> 8))
    end = 0x9821 + (len(text) // 18) * 32 + len(text) % 18
    code += bytes((0xaf, 0xea, end & 255, end >> 8))
    if lcd:
        code += bytes.fromhex('3e 91 e0 40')
    code += bytes.fromhex('18 fe' if terminal else '00 18 fd')
    data[0x150:0x150 + len(code)] = code
    data[0x14d] = (-sum(data[0x134:0x14d]) - 25) & 255
    return data


class Protocol(unittest.TestCase):
    def run_case(self, text, *args, **kwargs):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'authored.gb'
            path.write_bytes(rom(text, **kwargs))
            return subprocess.run([RUNNER, str(path), '--model', 'dmg',
                '--protocol', 'bullygb', '--max-cycles', '100000', *args],
                capture_output=True, text=True, timeout=20)

    def test_exact_success(self):
        result = self.run_case('All tests OK!')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PASS (BullyGB) result=All tests OK!', result.stdout)

    def test_wrapped_error(self):
        result = self.run_case('Invalid initial   DIV')
        self.assertEqual(result.returncode, 1)
        self.assertIn('FAIL (BullyGB) result=Invalid initial   DIV', result.stderr)

    def test_success_substring_is_failure(self):
        for text in ('All tests OK! INVALID', 'All tests OK!\x01'):
            result = self.run_case(text)
            self.assertEqual(result.returncode, 1)

    def test_incomplete_or_nonterminal_screen_times_out(self):
        for text, options in (('', {}), ('All tests OK!', dict(lcd=False)),
                              ('All tests OK!', dict(terminal=False))):
            result = self.run_case(text, **options)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertNotIn('PASS (BullyGB)', result.stdout)

    def test_opt_in_and_capture_are_preserved(self):
        result = self.run_case('All tests OK!', '--protocol', 'mooneye')
        self.assertEqual(result.returncode, 2)
        with tempfile.TemporaryDirectory() as tmp:
            output = str(Path(tmp) / 'capture.ppm')
            result = self.run_case('All tests OK!', '--cycle-frame-capture', '1', '--frame-output', output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('PASS (BullyGB)', result.stdout)
            self.assertTrue(Path(output).is_file())


if __name__ == '__main__':
    unittest.main()
