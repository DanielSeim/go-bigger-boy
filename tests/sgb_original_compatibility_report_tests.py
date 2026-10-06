# SPDX-License-Identifier: GPL-3.0-or-later
"""Demand inventory boundaries and bounded original-firmware probe contracts."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from report_sgb_original_compatibility import audit, describe

PROBE = None


def write_trace(path, packets):
    lines = ['GBB SGB trace']
    for sequence, packet in packets:
        lines.append(f'command sequence={sequence} command=0x{packet[0]>>3:02X} bytes={len(packet)} packet={packet.hex()}')
    path.write_text('\n'.join(lines + ['end', '']), encoding='utf-8')


def packet(*values):
    return bytes(values) + bytes(16-len(values))


class DemandContracts(unittest.TestCase):
    def test_upload_boundary_does_not_claim_score_compatibility(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'demand.trace'
            write_trace(path, [(1, packet(0x41, 0x80, 0x80, 0x8C)), (2, packet(0x49)),
                               (3, packet(0x41, 0, 0, 0, 1)), (4, packet(0x79))])
            result = audit(path)
            self.assertFalse(result['qualification'])
            self.assertEqual(result['first_transfer_sequence'], 2)
            self.assertEqual(result['parameter_gaps'], {})
            self.assertEqual(result['ignored_host_commands'], {'DATA_SND': 1})
            self.assertTrue(result['audio_events'][2]['parameters_in_subset'])
            self.assertEqual(result['audio_events'][2]['mailbox_evidence'], 'unverified_after_transfer_request')
            self.assertIn('require separate evidence', describe(result))
            self.assertNotIn('packet', json.dumps(result))

    def test_mailbox_ranges_and_reserved_attributes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bank.trace'
            write_trace(path, [(1, packet(0x41, 5, 5, 0x11, 2)), (2, packet(0x41, 6, 0, 0xC0, 3))])
            for version in range(1, 5):
                result = audit(path, version)
                first = result['audio_events'][0]
                self.assertEqual(first['parameters_in_subset'], version == 4)
                self.assertTrue(result['audio_events'][1]['gaps'])
            self.assertIn('reserved B volume', audit(path)['parameter_gaps'])
            self.assertIn('unsupported effect A 0x06', audit(path)['parameter_gaps'])
            self.assertIn('unsupported score 0x03', audit(path)['parameter_gaps'])
            self.assertIn('unsupported score 0x02', audit(path, 5)['parameter_gaps'])
            self.assertIn('unsupported score 0x02', audit(path, 6)['parameter_gaps'])
            self.assertIn('unsupported score 0x02', audit(path, 7)['parameter_gaps'])
            self.assertIn('unsupported score 0x02', audit(path, 8)['parameter_gaps'])
            self.assertIn('unsupported score 0x02', audit(path, 9)['parameter_gaps'])
            self.assertIn('unsupported score 0x02', audit(path, 10)['parameter_gaps'])
            for version in (0, 11, True):
                with self.assertRaises(ValueError):
                    audit(path, version)

    def test_malformed_framing_and_sequence_order(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.trace'
            write_trace(path, [(1, packet(0x42)), (2, packet(0x4A))])
            result = audit(path)
            self.assertIn('unsupported SOUND framing', result['parameter_gaps'])
            self.assertIn('unsupported SOU_TRN framing', result['parameter_gaps'])
            write_trace(path, [(2, packet(0x41)), (1, packet(0x41))])
            with self.assertRaises(ValueError):
                audit(path)

    def test_cli_emits_inventory_json(self):
        fixture = ROOT / 'tests/fixtures/sgb/trace_fixture.trace'
        result = subprocess.run([sys.executable, str(ROOT/'scripts/report_sgb_original_compatibility.py'),
                                 str(fixture), '--json'], check=True, capture_output=True, text=True)
        report = json.loads(result.stdout)[0]
        self.assertFalse(report['qualification'])
        self.assertEqual(report['command_counts'], {'0x11': 1})
        self.assertEqual(report['audio_events'], [])

    def test_probe_uses_real_firmware_and_reports_halt_without_qualification(self):
        if PROBE is None:
            self.skipTest('supply --probe for the native firmware integration check')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'unsupported.gb'
            rom = bytearray(32768);rom[0x100:0x103] = bytes.fromhex('c35001');rom[0x146] = 3
            code = bytearray()
            def joy(value):
                code.extend((0x3E, value, 0xE0, 0))
            joy(0x30);joy(0);joy(0x30)
            for value in packet(0x41, 6):
                for bit in range(8):
                    joy(0x10 if value & (1<<bit) else 0x20);joy(0x30)
            joy(0x20);joy(0x30);code.extend((0x18,0xFE))
            rom[0x150:0x150+len(code)] = code;path.write_bytes(rom)
            for model in ('sgb', 'sgb2'):
                completed = subprocess.run([str(PROBE), model, str(path), '20000000'],
                                           check=True, capture_output=True, text=True)
                result = json.loads(completed.stdout)
                self.assertFalse(result['qualification'])
                self.assertEqual(result['outcome'], 'prototype_halt')
                self.assertEqual(result['unsupported_header'], 0x41)
                self.assertEqual(result['firmware_state'], 0xFF)
                self.assertEqual(result['captures'], 0)
                self.assertEqual(result['mailbox_version'], 0xC4)
                self.assertLess(result['master_clocks'], 20000000)
            clock = subprocess.run([str(PROBE), 'sgb2', str(path), '1'], check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(clock.stdout)['outcome'], 'clock_limit')
            bad = subprocess.run([str(PROBE), 'sgb2', str(path), '0'], capture_output=True)
            self.assertEqual(bad.returncode, 2)

    def test_probe_reports_captured_metadata_without_dumping_payload(self):
        if PROBE is None:
            self.skipTest('supply --probe for the native firmware integration check')
        from build_sgb_score_transfer import build
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'score-upload.gb'
            rom = bytearray(32768);rom[0x100:0x103] = bytes.fromhex('c35001');rom[0x146] = 3
            rom[0x4000:0x5000] = build({'scores': [[8]*8, [8]*8]})
            code = bytearray.fromhex('f3afe0401100402100800100101a22130b78b120f8')
            for row in range(13):
                address = 0x9800 + row*32
                code.extend((0x21, address&255, address>>8))
                for x in range(20):
                    index = row*20 + x
                    if index < 256:
                        code.extend((0x3E,index,0x22))
            code.extend((0x3E,0xE4,0xE0,0x47,0x3E,0x91,0xE0,0x40))
            def joy(value):
                code.extend((0x3E,value,0xE0,0))
            joy(0x30);joy(0);joy(0x30)
            for value in packet(0x49):
                for bit in range(8):
                    joy(0x10 if value&(1<<bit) else 0x20);joy(0x30)
            joy(0x20);joy(0x30);code.extend((0x18,0xFE))
            rom[0x150:0x150+len(code)] = code;path.write_bytes(rom)
            completed = subprocess.run([str(PROBE), 'sgb2', str(path), '20000000'],
                                       check=True, capture_output=True, text=True)
            result = json.loads(completed.stdout)
            self.assertFalse(result['qualification'])
            self.assertEqual(result['outcome'], 'clock_limit')
            self.assertEqual(result['captured_writes'], [{'destination': 0x07D0, 'size': 16}])
            self.assertEqual(result['captured_jump'], 0x0200)
            self.assertEqual(result['captured_list_error'], 'none')
            self.assertEqual(result['adoptions'], 2)
            self.assertEqual(result['external_owner'], 0)
            self.assertEqual(result['fault_pc'], 0)
            self.assertNotIn('payload', result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args = parser.parse_args()
    PROBE = args.probe
    unittest.main(argv=[sys.argv[0]])
