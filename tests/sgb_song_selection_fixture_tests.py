#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Original cartridge/transport fixtures and fail-closed reference reporting."""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_sgb_song_selection_fixture import build, score_payload
from check_sgb_song_selection_reference import check_reads, run
from decode_sgb_score import decode
from report_sgb_commands import read_commands
RUNNER = None


def observation(order):
    events = []
    for number, song in enumerate(order, 1):
        low = 0x2B00 + 2 * (song - 1)
        for address in (low + 1, low):
            tick = len(events) + 1
            events.append({'address': address, 'master_clock': tick * 20,
                           'spc_half_clock': tick, 'audible_packets': number, 'base_writes': 2})
    return {'schema': 'gbb-sgb-score-table-reads-v1', 'qualification': False,
            'overflow': False, 'base': 0x2B00, 'bytes': 6, 'capacity': 256,
            'reads': len(events), 'events': events}


class SongSelectionFixtures(unittest.TestCase):
    def test_header_transport_and_distinct_empty_roots(self):
        rom = build()
        self.assertEqual(len(rom), 32768)
        self.assertEqual(rom[0x100:0x103], bytes.fromhex('c35001'))
        self.assertEqual(rom[0x134:0x13E], b'GBB SELECT')
        self.assertEqual((rom[0x146], rom[0x147], rom[0x14B]), (3, 0, 0x33))
        self.assertEqual((sum(rom[0x134:0x14E]) + 25) & 255, 0)
        self.assertEqual(int.from_bytes(rom[0x14E:0x150], 'big'), sum(rom[:0x14E] + rom[0x150:]) & 65535)
        payload = score_payload()
        self.assertEqual(rom[0x4000:0x5000], payload)
        self.assertEqual(struct.unpack_from('<HH', payload), (32, 0x2B00))
        self.assertEqual(struct.unpack_from('<HH', payload, 36), (0, 0x0400))
        bank = payload[4:36]
        self.assertEqual(struct.unpack_from('<HHH', bank), (0x2B10, 0x2B14, 0x2B18))
        for root in (0x2B10, 0x2B14, 0x2B18):
            result = decode(bank, root)
            self.assertEqual(result['patterns'], [])
            self.assertEqual(result['event_count'], 0)
        self.assertEqual(payload[40:], bytes(4096-40))

    def test_order_bounds_and_determinism(self):
        self.assertEqual(build(), build([1, 2, 3]))
        self.assertNotEqual(build([1, 2, 3]), build([3, 2, 1]))
        self.assertEqual(len(build([3] * 8)), 32768)
        for order in (None, [], '123', [0], [4], [True], [1.0], ['1'], [1] * 9):
            with self.subTest(order=order), self.assertRaises(ValueError):
                build(order)

    def test_reference_pair_rules_and_repeated_ids(self):
        for order in ([1, 2, 3], [3, 1, 2, 2, 1, 3], [1] * 8):
            selected = check_reads(observation(order), order)
            self.assertEqual([event['word_address'] for event in selected], [0x2B00+2*(song-1) for song in order])
        report = observation([1, 2, 3])
        invalid = [None, {}, dict(report, overflow=True), dict(report, qualification=True),
                   dict(report, reads=5), dict(report, events=[]), dict(report, base=0x2B02)]
        for key, value in (('address', 0x2B02), ('base_writes', 1), ('audible_packets', 0),
                           ('master_clock', True), ('spc_half_clock', '1')):
            changed = deepcopy(report)
            changed['events'][0][key] = value
            invalid.append(changed)
        changed = deepcopy(report)
        changed['events'][0]['value'] = 99
        invalid.append(changed)
        changed = deepcopy(report)
        changed['events'][1]['spc_half_clock'] = 0
        invalid.append(changed)
        changed = deepcopy(report)
        for event in changed['events'][2:]:
            event['base_writes'] = 3
        invalid.append(changed)
        for changed in invalid:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                check_reads(changed, [1, 2, 3])

    def test_failed_child_diagnostics_are_not_forwarded(self):
        failed = subprocess.CompletedProcess([], 3, b'private stdout sentinel', b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory, patch('check_sgb_song_selection_reference.subprocess.run', return_value=failed):
            with self.assertRaises(ValueError) as error:
                run(Path('unused-trace'), Path(directory), 'sgb', [1])
            self.assertEqual(str(error.exception), 'sgb: reference did not finish at the instruction bound')

    def test_success_report_contains_only_contract_metadata(self):
        def child(command, **kwargs):
            report = observation([3, 1])
            report['private_blob'] = 'private data sentinel'
            Path(command[-1]).write_text(json.dumps(report))
            return subprocess.CompletedProcess(command, 4, b'private stdout sentinel', b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / 'sgb1.program.rom').write_bytes(b'original test dummy')
            (base / 'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_song_selection_reference.subprocess.run', side_effect=child):
                result = run(Path('unused-trace'), base, 'sgb', [3, 1])
            self.assertEqual(set(result), {'model', 'observations', 'fixture_sha256', 'gb_boot_sha256', 'program_sha256', 'ipl_sha256'})
            self.assertEqual(result['observations'], [{'request': 1, 'code': 3, 'word_address': 0x2B04},
                                                     {'request': 2, 'code': 1, 'word_address': 0x2B00}])
            self.assertNotIn('sentinel', json.dumps(result))

    def test_cli_no_overwrite_invalid_order_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fixture.gb'
            command = [sys.executable, str(ROOT / 'scripts/build_sgb_song_selection_fixture.py'), '--output', str(path)]
            subprocess.run(command, check=True, capture_output=True)
            expected = path.read_bytes()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            self.assertEqual(path.read_bytes(), expected)
            path.unlink()
            for order in ('', '0', '4', 'one', ',1', ','.join(['1'] * 9)):
                self.assertEqual(subprocess.run(command + ['--order', order], capture_output=True).returncode, 2)
                self.assertFalse(path.exists())
            checker = [sys.executable, str(ROOT / 'scripts/check_sgb_song_selection_reference.py'),
                       '--trace', 'missing-trace', '--firmware-dir', directory]
            result = subprocess.run(checker, capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b'')

    def test_real_gameboy_joyp_command_order(self):
        if RUNNER is None:
            self.skipTest('supply --runner for actual GB execution')
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            game = base / 'fixture.gb'
            order = [3, 1, 2, 2, 1, 3]
            game.write_bytes(build(order))
            for model in ('sgb', 'sgb2'):
                trace = base / f'{model}.trace'
                subprocess.run([str(RUNNER), str(game), '--model', model, '--frames', '160',
                                '--frame-output', str(base / f'{model}.ppm'), '--sgb-trace', str(trace),
                                '--max-cycles', '16000000'], check=True, capture_output=True)
                packets = [r.packet for r in read_commands(trace) if r.command in (8, 9)]
                self.assertEqual(packets, [bytes([0x49]) + bytes(15)] +
                                 [bytes([0x41, 0, 0, 0, song]) + bytes(11) for song in order])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', type=Path)
    RUNNER = parser.parse_args().runner
    unittest.main(argv=[sys.argv[0]])
