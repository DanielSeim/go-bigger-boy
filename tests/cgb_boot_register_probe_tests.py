# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('probe', ROOT / 'scripts/cgb_boot_register_probe.py')
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)


class BootRegisterContracts(unittest.TestCase):
    def test_authored_headers_and_optional_private_logo(self):
        for flag in (0, 0x80, 0xc0):
            rom = PROBE.probe_rom(flag)
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            self.assertEqual(rom[0x143], flag)
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        self.assertEqual(PROBE.probe_rom(0x80, bytes(range(48)))[0x104:0x134], bytes(range(48)))
        for flag, logo in ((1, None), (0, b''), (0, bytes(49))):
            with self.assertRaises(ValueError):
                PROBE.probe_rom(flag, logo)

    @staticmethod
    def fake_run(command, **kwargs):
        reference = '--boot-registers' in command
        rom = Path(command[2] if reference else command[1]).read_bytes()
        native = bool(rom[0x143])
        registers = dict(af=0x1180, bc=0, de=0xff56 if native else 8,
                         hl=0x0d if native else 0x7c, sp=0xfffe, pc=0x100)
        if reference:
            data = dict(schema='gbb-sameboy-cgb-boot-registers-v1', qualification=False,
                        model=command[4], result='handoff', clocks_8mhz=100,
                        anchor='boot_unmapped_before_cartridge_entry', **registers)
            return subprocess.CompletedProcess(command, 0, json.dumps(data), '')
        fields = ' '.join(f'{k}={v:04x}' for k, v in registers.items() if k != 'pc')
        line = ('CPU_FIRST_PC cycle=0 duration=0 pc=none opcode=none next_pc=0100 '
                f'{fields} normalized_8mhz=0 transition=initial\n')
        return subprocess.CompletedProcess(command, 0, 'PASS (Mooneye)\n', line)

    def run_mocked(self, callback):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / 'not-executed'
            file.write_bytes(b'owned mock fixture')
            with patch.object(PROBE.subprocess, 'run', side_effect=callback):
                return PROBE.run_probe(file, file, file, 'cgb-e', Path(tmp) / 'result')

    def test_three_modes_and_two_takes(self):
        report = self.run_mocked(self.fake_run)
        self.assertFalse(report['qualification'])
        self.assertEqual(len(report['commands']), 12)
        self.assertEqual([r['cgb_flag'] for r in report['rows']], [0, 0x80, 0xc0])
        self.assertTrue(all(r['match'] and r['repeatable'] for r in report['rows']))

    def test_native_compatibility_mixup_is_a_real_mismatch(self):
        def wrong(command, **kwargs):
            result = self.fake_run(command)
            if '--boot-registers' not in command:
                result.stderr = result.stderr.replace('de=ff56', 'de=0008').replace('hl=000d', 'hl=007c')
            return result
        report = self.run_mocked(wrong)
        self.assertEqual([r['match'] for r in report['rows']], [True, False, False])

    def test_reference_failure_and_bad_metadata_fail_closed(self):
        for change in ({'result': 'timeout'}, {'pc': 0x150}, {'qualification': 0},
                       {'de': True}, {'model': 'cgb0'}, {'clocks_8mhz': 0}):
            def wrong(command, **kwargs):
                result = self.fake_run(command)
                if '--boot-registers' in command:
                    data = json.loads(result.stdout)
                    data.update(change)
                    result.stdout = json.dumps(data)
                return result
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.run_mocked(wrong)
        with self.assertRaises(ValueError):
            self.run_mocked(lambda *a, **kw: subprocess.CompletedProcess([], 3, '', 'timeout'))

    def test_a_later_or_ambiguous_gbb_pc_hit_is_rejected(self):
        for change in ('later', 'duplicate'):
            def wrong(command, **kwargs):
                result = self.fake_run(command)
                if '--boot-registers' not in command:
                    result.stderr = (result.stderr * 2 if change == 'duplicate' else
                                     result.stderr.replace('transition=initial', 'transition=instruction'))
                return result
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.run_mocked(wrong)


if __name__ == '__main__':
    unittest.main()
