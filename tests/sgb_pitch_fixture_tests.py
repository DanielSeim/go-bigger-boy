#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned note transport and bounded, sanitized DSP reference contracts."""
import hashlib
import io
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
from build_sgb_pitch_fixture import build, score_payload
from check_sgb_pitch_reference import observe, check_contract, run
from build_sgb_song_selection_fixture import build_cartridge

HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def events(pitches=(1068, 1132, 2140)):
    writes = [(0x3D, 0), (0x24, 2), (0x25, 0x8F), (0x26, 0x6F), (0x27, 0xB8)]
    for pitch in pitches:
        writes.extend(((0x22, pitch & 255), (0x23, pitch >> 8), (0x4C, 4)))
    return writes


def trace(writes):
    return HEADER + ''.join(f'D,0,{index},0,{address},{value}\n' for index, (address, value) in enumerate(writes))


class PitchFixtures(unittest.TestCase):
    def test_transport_channel_and_notes(self):
        pins = {2: 'b189855a06911879d605c71dc94950fa264a6c6399a15a2a6ab9a2b0894f4f94',
                10: '9493e26e50daece6a6ffe78567a60b281833e684637dc0f241630ebd8bb10c8b'}
        for instrument in (2, 10):
            rom, payload = build(instrument), score_payload(instrument)
            self.assertEqual(hashlib.sha256(rom).hexdigest(), pins[instrument])
            self.assertEqual(rom[0x4000:0x5000], payload)
            self.assertEqual((sum(rom[0x134:0x14E]) + 25) & 255, 0)
            self.assertEqual(struct.unpack_from('<HH', payload), (128, 0x2B00))
            self.assertEqual(struct.unpack_from('<HH', payload, 132), (0, 0x0400))
            bank = payload[4:132]
            for index, note in enumerate((24, 25, 36)):
                root = struct.unpack_from('<H', bank, 2*index)[0] - 0x2B00
                table, end = struct.unpack_from('<HH', bank, root)
                self.assertEqual(end, 0)
                channels = struct.unpack_from('<8H', bank, table-0x2B00)
                self.assertEqual([i for i, pointer in enumerate(channels) if pointer], [2])
                offset = channels[2] - 0x2B00
                self.assertEqual(bank[offset:offset+16], bytes((0xE0, instrument, 0xE1, 10, 0xE5, 160,
                                 0xED, 127, 0xE7, 96, 16, 0x7F, 0x80+note, 1, 0xC9, 0)))
            self.assertEqual(payload[136:], bytes(4096-136))
        for instrument in (True, None, 2.0, '2', 0, 11):
            with self.assertRaises(ValueError):
                build(instrument)
        for payload in (b'', bytes(4095), bytearray(4096), None):
            with self.assertRaises(ValueError):
                build_cartridge(payload, (1,))

    def test_dsp_metadata_and_contract(self):
        result = observe(io.StringIO(trace(events()) + 'R,0,99,0,100,private sample sentinel\n'))
        check_contract(result, 2)
        self.assertEqual([note['pitch'] for note in result['notes']], [1068, 1132, 2140])
        self.assertEqual([note['base_note'] for note in result['notes']], [24, 25, 36])
        self.assertNotIn('sentinel', json.dumps(result))
        # Correct intervals alone must not permit a different tuning contract.
        changed = observe(io.StringIO(trace(events((1200, 1272, 2400)))))
        with self.assertRaises(ValueError):
            check_contract(changed, 2)

    def test_fail_closed_incomplete_unordered_or_wrong_voice(self):
        invalid = [HEADER, 'bad header\n', trace(events()[:-1]), trace(events()+[(0x4C, 4)]),
                   trace(events((0, 1132, 2140))), trace(events((1068, 1500, 2140))),
                   trace(events((1068, 1132, 0x4000))), trace(events()[1:]),
                   trace([(0x3D, 4)] + events()[1:]), trace(events()).replace(',76,4\n', ',76,8\n'),
                   trace(events()).replace('D,0,1,', 'D,0,-1,'),
                   trace(events()).replace('D,0,2,', 'D,0,0,'),
                   HEADER+'D,0,0,0,34\n', HEADER+'D,0,0,0,34,1,extra\n',
                   HEADER+'D,0,0,0,128,0\n', HEADER+'D,0,0,0,34,256\n',
                   trace(events()+[(0x24, 10), (0x4C, 4)])]
        changed = events()
        changed.insert(8, (0x24, 3))
        invalid.append(trace(changed))
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(ValueError):
                observe(io.StringIO(data))
        with patch('check_sgb_pitch_reference.MAX_ROWS', 2), self.assertRaises(ValueError):
            observe(io.StringIO(HEADER+'R,0,0,0,0,private\n'*2))

    def test_failed_child_diagnostics_are_not_forwarded(self):
        failed = subprocess.CompletedProcess([], 3, b'private stdout sentinel', b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory, patch('check_sgb_pitch_reference.subprocess.run', return_value=failed):
            with self.assertRaises(ValueError) as error:
                run(Path('unused-trace'), Path(directory), 'sgb', 2)
            self.assertEqual(str(error.exception), 'sgb: reference did not finish at the instruction bound')

    def test_success_report_contains_only_note_metadata(self):
        def child(command, **kwargs):
            Path(command[-1]).write_text(trace(events())+'R,0,99,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command, 4, b'private stdout sentinel', b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base/'sgb1.program.rom').write_bytes(b'original test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_pitch_reference.subprocess.run', side_effect=child):
                result = run(Path('unused-trace'), base, 'sgb', 2)
            self.assertEqual(set(result), {'model', 'instrument', 'notes', 'semitone_ratio', 'octave_ratio',
                             'fixture_sha256', 'gb_boot_sha256', 'program_sha256', 'ipl_sha256'})
            self.assertNotIn('sentinel', json.dumps(result))

    def test_cli_no_overwrite_invalid_instrument_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'fixture.gb'
            command = [sys.executable, str(ROOT/'scripts/build_sgb_pitch_fixture.py'), '--output', str(path)]
            subprocess.run(command, check=True, capture_output=True)
            expected = path.read_bytes()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            self.assertEqual(path.read_bytes(), expected)
            path.unlink()
            self.assertEqual(subprocess.run(command+['--instrument', '3'], capture_output=True).returncode, 2)
            self.assertFalse(path.exists())
            result = subprocess.run([sys.executable, str(ROOT/'scripts/check_sgb_pitch_reference.py'),
                                     '--trace', 'missing-trace', '--firmware-dir', directory], capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b'')


if __name__ == '__main__':
    unittest.main()
