#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned control-isolation transport and sanitized voice-volume contracts."""
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
sys.path.insert(0, str(ROOT/'scripts'))
from build_sgb_volume_pan_fixture import build, score_payload, CASES
from check_sgb_volume_pan_reference import observe, check_contract, run

HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(volumes=((7, 7), (0, 11), (11, 0))):
    writes = [(0x3D, 0), (0x24, 2), (0x22, 44), (0x23, 4)]
    for left, right in volumes:
        writes.extend(((0x20, left), (0x21, right), (0x4C, 4)))
    return HEADER + ''.join(f'D,0,{index},0,{address},{value}\n' for index, (address,value) in enumerate(writes))


class VolumePanFixtures(unittest.TestCase):
    def test_transport_channel_controls_and_pins(self):
        pins = {'pan': '00d139e36d83e474560d35682252bb059f92527f17b916300d04261ae42b1da1',
                'volume': 'ba62f8a1f83679d2f83dd704245ff23f5f3c09a0e33e293ba8590e22dd464b04'}
        for case, controls in CASES.items():
            rom, payload = build(case), score_payload(case)
            self.assertEqual(hashlib.sha256(rom).hexdigest(), pins[case])
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x4000:0x5000], payload)
            self.assertEqual((sum(rom[0x134:0x14E]) + 25) & 255, 0)
            self.assertEqual(int.from_bytes(rom[0x14E:0x150], 'big'), sum(rom[:0x14E]+rom[0x150:]) & 65535)
            self.assertEqual(struct.unpack_from('<HH', payload), (128, 0x2B00))
            self.assertEqual(struct.unpack_from('<HH', payload, 132), (0, 0x0400))
            bank = payload[4:132]
            for index, (pan, song_volume, track_volume) in enumerate(controls):
                root = struct.unpack_from('<H', bank, index*2)[0]-0x2B00
                table, end = struct.unpack_from('<HH', bank, root)
                self.assertEqual(end, 0)
                channels = struct.unpack_from('<8H', bank, table-0x2B00)
                self.assertEqual([i for i, pointer in enumerate(channels) if pointer], [2])
                offset = channels[2]-0x2B00
                self.assertEqual(bank[offset:offset+16], bytes((0xE0,2,0xE1,pan,0xE5,song_volume,
                                 0xED,track_volume,0xE7,96,16,0x7F,0x98,1,0xC9,0)))
            # Only one operand changes relative to each explicitly restored baseline.
            baseline = bank[80:96]
            for index, operand in enumerate((3,3) if case=='pan' else (5,7), 1):
                changed = bank[80+index*16:96+index*16]
                self.assertEqual([i for i,(a,b) in enumerate(zip(baseline,changed)) if a!=b], [operand])
            self.assertEqual(payload[136:], bytes(4096-136))
        with self.assertRaises(ValueError):
            build('missing')

    def test_metadata_and_exact_contract(self):
        for case, values in (('pan', ((7,7),(0,11),(11,0))), ('volume', ((7,7),(1,1),(1,1)))):
            result = observe(io.StringIO(trace(values)+'R,0,99,0,100,private sample sentinel\n'))
            check_contract(result, case)
            self.assertEqual([(n['voll'],n['volr']) for n in result['notes']], list(values))
            self.assertNotIn('sentinel', json.dumps(result))
        for values in (((7,7),(11,0),(0,11)), ((7,7),(3,3),(3,3))):
            result = observe(io.StringIO(trace(values)))
            with self.assertRaises(ValueError):
                check_contract(result, 'pan' if values[1]==(11,0) else 'volume')

    def test_rejects_missing_wrong_source_voice_or_malformed_notes(self):
        data = trace()
        invalid = [HEADER, 'bad header\n', trace(((7,7),(0,11))), trace(((7,7),)*4),
                   data.replace(',76,4\n', ',76,8\n'), data.replace('D,0,0,0,61,0', 'D,0,0,0,61,4'),
                   data.replace('D,0,1,0,36,2', 'D,0,1,0,36,10'),
                   data.replace('D,0,2,0,34,44', 'D,0,2,0,34,45'),
                   data.replace('D,0,5,0,33,7\n', ''), data.replace('D,0,5,', 'D,0,3,'),
                   HEADER+'D,0,-1,0,34,0\n', HEADER+'D,0,0,0,128,0\n',
                   HEADER+'D,0,0,0,34,256\n', HEADER+'D,0,0,0,34\n',
                   HEADER+'D,0,0,0,34,0,extra\n']
        for changed in invalid:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                observe(io.StringIO(changed))
        with patch('check_sgb_volume_pan_reference.MAX_ROWS', 2), self.assertRaises(ValueError):
            observe(io.StringIO(HEADER+'R,0,0,0,0,private\n'*2))

    def test_success_report_contains_only_control_and_note_metadata(self):
        def child(command, **kwargs):
            Path(command[-1]).write_text(trace()+'R,0,99,0,100,private sample sentinel\n')
            return subprocess.CompletedProcess(command,4,b'private stdout sentinel',b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            (base/'sgb1.program.rom').write_bytes(b'original test dummy')
            (base/'spc700.rom').write_bytes(bytes(64))
            with patch('check_sgb_volume_pan_reference.subprocess.run',side_effect=child):
                result=run(Path('unused-trace'),base,'sgb','pan')
            self.assertEqual(set(result), {'model','case','notes','fixture_sha256','gb_boot_sha256','program_sha256','ipl_sha256'})
            self.assertEqual([n['request'] for n in result['notes']], [1,2,3])
            self.assertEqual([n['pan'] for n in result['notes']], [10,0,20])
            self.assertNotIn('sentinel',json.dumps(result))

    def test_failed_child_diagnostics_are_not_forwarded(self):
        failed=subprocess.CompletedProcess([],3,b'private stdout sentinel',b'private stderr sentinel')
        with tempfile.TemporaryDirectory() as directory, patch('check_sgb_volume_pan_reference.subprocess.run',return_value=failed):
            with self.assertRaises(ValueError) as error:
                run(Path('unused-trace'),Path(directory),'sgb','pan')
            self.assertEqual(str(error.exception),'sgb: reference did not finish at the instruction bound')

    def test_cli_no_overwrite_invalid_case_and_no_partial_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'fixture.gb'
            command=[sys.executable,str(ROOT/'scripts/build_sgb_volume_pan_fixture.py'),'--output',str(path)]
            subprocess.run(command,check=True,capture_output=True)
            expected=path.read_bytes()
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(path.read_bytes(),expected)
            path.unlink()
            self.assertEqual(subprocess.run(command+['--case','missing'],capture_output=True).returncode,2)
            self.assertFalse(path.exists())
            result=subprocess.run([sys.executable,str(ROOT/'scripts/check_sgb_volume_pan_reference.py'),
                                   '--trace','missing-trace','--firmware-dir',directory],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')


if __name__ == '__main__':
    unittest.main()
