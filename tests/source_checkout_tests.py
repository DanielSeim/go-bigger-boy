#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Preserve pinned provenance bytes through Windows-style Git checkouts."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    'scripts/cgb_pixel_phase_probe.py',
    'scripts/sameboy_pixel_access_trace.c',
    'scripts/gbb_pixel_access_trace.cpp',
    'scripts/pixel_access_trace.h',
    'src/ppu_timing.cpp',
    'include/gameboy/ppu.hpp',
    'tests/fixtures/research/cgb-native-obj-cancellation-rejected.patch',
)


@unittest.skipUnless(shutil.which('git'), 'Git is required to simulate checkout filters')
class SourceCheckoutContracts(unittest.TestCase):
    def checkout(self, attributes):
        with tempfile.TemporaryDirectory(prefix='gbb source checkout ') as directory:
            root = Path(directory)
            repo, output = root / 'repo', root / 'checkout'
            repo.mkdir()
            subprocess.run(['git', 'init', '--quiet', str(repo)], check=True, capture_output=True)
            (repo / '.gitattributes').write_bytes(attributes)
            for source in SOURCES:
                path = repo / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((ROOT / source).read_bytes())
            subprocess.run(['git', '-C', str(repo), '-c', 'core.autocrlf=true',
                            'add', '--', '.gitattributes', *SOURCES], check=True, capture_output=True)
            subprocess.run(['git', '-C', str(repo), '-c', 'core.autocrlf=true',
                            'checkout-index', '--all', '--prefix=' + output.as_posix() + '/'],
                           check=True, capture_output=True)
            return {source: (output / source).read_bytes() for source in SOURCES}

    def test_windows_style_checkout_preserves_every_pinned_source_format(self):
        checked_out = self.checkout((ROOT / '.gitattributes').read_bytes())
        for source, actual in checked_out.items():
            with self.subTest(source=source):
                expected = (ROOT / source).read_bytes()
                self.assertEqual(actual, expected)
                self.assertEqual(hashlib.sha256(actual).digest(), hashlib.sha256(expected).digest())
                self.assertNotIn(b'\r\n', actual)

    def test_unprotected_checkout_reproduces_hash_failure(self):
        checked_out = self.checkout(b'')
        for source, actual in checked_out.items():
            with self.subTest(source=source):
                expected = (ROOT / source).read_bytes()
                self.assertNotEqual(hashlib.sha256(actual).digest(), hashlib.sha256(expected).digest())
                self.assertEqual(actual.replace(b'\r\n', b'\n'), expected)


if __name__ == '__main__':
    unittest.main()
