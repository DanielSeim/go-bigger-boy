# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cgb_stat_clock_probe as probe
RUNNER = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / 'build/gbb_stat_clock_trace'
EVIDENCE = ROOT / 'tests/fixtures/external-reference/cgb-stat-clock.json'


class StatClockContracts(unittest.TestCase):
    def test_owned_roms_headers_and_bounded_calibration(self):
        roms = [probe.probe_rom(s, speed) for speed in (False, True) for s in probe.SCENARIOS]
        self.assertEqual(len(set(roms)), 66)
        for rom in roms:
            self.assertEqual(len(rom), 32768)
            self.assertEqual(rom[0x104:0x134], bytes(48))
            checksum = 0
            for value in rom[0x134:0x14d]:
                checksum = (checksum - value - 1) & 255
            self.assertEqual(rom[0x14d], checksum)
            self.assertEqual(int.from_bytes(rom[0x14e:0x150], 'big'),
                             sum(rom[:0x14e] + rom[0x150:]) & 65535)
        rom = probe.probe_rom('stat-nop')
        self.assertEqual(rom[0x1000:0x1100], bytes.fromhex('3e 87 e0 40') + bytes(252))
        self.assertEqual(rom[0x1100], 0xf3)
        with self.assertRaises(ValueError):
            probe.probe_rom('unknown')

    def test_recorded_evidence_provenance_and_complete_case_grid(self):
        evidence = json.loads(EVIDENCE.read_text())
        self.assertFalse(evidence['qualification'])
        for path, digest in evidence['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest)
        rows = evidence['rows']
        self.assertEqual(len(rows), 66)
        self.assertEqual({(r['scenario'], r['double_speed']) for r in rows},
                         {(s, speed) for s in probe.SCENARIOS for speed in (False, True)})
        for row in rows:
            self.assertEqual(row['rom_sha256'], hashlib.sha256(
                probe.probe_rom(row['scenario'], row['double_speed'])).hexdigest())
            self.assertEqual(row['match'], row['captures']['gbb']['witness'] == row['captures']['sameboy']['witness'])
            self.assertTrue(all(c['repeatable'] and c['observer_equivalent'] for c in row['captures'].values()))
        self.assertEqual(sum(r['match'] for r in rows), evidence['summary']['matches'])
        for example in evidence['examples']:
            probe.validate_trace(example['trace'], example['scenario'], example['double_speed'], example['trace']['core'])

    def test_elapsed_capture_flushes_owned_pixels_without_changing_clocks(self):
        evidence = json.loads(EVIDENCE.read_text())['reference_elapsed_sync_calibration']
        self.assertTrue(evidence['repeatable'])
        self.assertTrue(evidence['legacy_clock_and_metadata_unchanged'])
        self.assertEqual(evidence['invalid_target_cases_rejected'], 5)
        self.assertEqual(evidence['rom_sha256'], hashlib.sha256(probe.anchor.probe_rom()).hexdigest())
        self.assertEqual(evidence['boot_sha256'], hashlib.sha256(probe.anchor.owned_boot()).hexdigest())
        rows = evidence['captures']
        self.assertEqual(len(rows), 8)
        self.assertEqual([r['metadata']['target_8mhz'] for r in rows], list(range(900000,900897,128)))
        self.assertEqual(sum(r['metadata']['pixels_flushed'] for r in rows), 47)
        for row in rows:
            metadata = row['metadata']
            self.assertEqual(metadata['schema'], 'gbb-sameboy-elapsed-capture-v1')
            self.assertFalse(metadata['qualification'])
            self.assertIs(metadata['ppu_synchronized'], True)
            self.assertEqual(metadata['anchor'], 'elapsed_8mhz_after_boot_unmap_pc_0100')
            self.assertLessEqual(metadata['target_8mhz'], metadata['clocks_8mhz'])
            self.assertLess(metadata['clocks_8mhz'] - metadata['target_8mhz'], 128)
            self.assertEqual(row['sha256_synced'] != row['sha256_unsynced'], metadata['pixels_flushed'] > 0)

    def example(self):
        example = json.loads(EVIDENCE.read_text())['examples'][0]
        return example['trace'], example['scenario'], example['double_speed']

    def test_invalid_metadata_intervals_and_dispatch_fail_closed(self):
        trace, scenario, speed = self.example()
        for key, value in (('complete', False), ('overflow', True), ('clock_unit', 'cpu-t'),
                           ('qualification', True), ('model', 'cgb-c'), ('observers_enabled', False),
                           ('request_lower_exclusive', trace['request_upper_inclusive']),
                           ('state_bytes', 0), ('state_fnv1a64', True)):
            changed = copy.deepcopy(trace)
            changed[key] = value
            with self.assertRaises(ValueError, msg=key):
                probe.validate_trace(changed, scenario, speed, trace['core'])
        for change in ('stack', 'time', 'dispatch', 'too-many-steps'):
            changed = copy.deepcopy(trace)
            if change == 'stack': changed['wram'][0x12] ^= 1
            elif change == 'time': changed['steps'][1]['begin'] += 1
            elif change == 'dispatch': changed['dispatch']['if_before'] = 1
            else: changed['steps'] *= 100
            with self.assertRaises(ValueError, msg=change):
                probe.validate_trace(changed, scenario, speed, trace['core'])

    def test_runtime_observers_preserve_serialized_state_and_registers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot = root / 'boot.bin'
            boot.write_bytes(probe.anchor.owned_boot())
            for speed in (False, True):
                for scenario in ('stat-nop', 'stat-original-phase0', 'software-incjr-phase0'):
                    with self.subTest(speed=speed, scenario=scenario):
                        rom = root / 'probe.gbc'
                        rom.write_bytes(probe.probe_rom(scenario, speed))
                        traces = []
                        for enabled in (True, False):
                            cmd = [str(RUNNER), str(rom), str(boot)] + ([] if enabled else ['--no-observers'])
                            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            trace = probe.validate_trace(json.loads(result.stdout), scenario, speed, 'gbb', enabled)
                            traces.append(trace)
                        self.assertEqual(probe.architectural_state(traces[0]), probe.architectural_state(traces[1]))

    def test_runtime_overflow_fails_without_unbounded_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot, rom = root / 'boot.bin', root / 'loop.gbc'
            boot.write_bytes(probe.anchor.owned_boot())
            data = bytearray(probe.probe_rom('stat-nop'))
            data[0x1000:0x1002] = bytes.fromhex('18 fe')
            rom.write_bytes(data)
            result = subprocess.run([str(RUNNER), str(rom), str(boot)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 3)
            self.assertLess(len(result.stdout), 150000)
            trace = json.loads(result.stdout)
            self.assertTrue(trace['overflow'])
            self.assertFalse(trace['complete'])
            self.assertEqual(len(trace['steps']), probe.MAX_STEPS)
            invalid = subprocess.run([str(RUNNER), str(rom), str(boot), '--unknown'], capture_output=True, timeout=30)
            self.assertEqual(invalid.returncode, 2)
            # STOP before the measured window leaves normalized time stalled.
            # The standalone tool must bound zero-progress steps as well.
            data = bytearray(probe.probe_rom('stat-nop'))
            data[0x100:0x102] = bytes.fromhex('10 00')
            rom.write_bytes(data)
            stopped = subprocess.run([str(RUNNER), str(rom), str(boot)], capture_output=True, text=True, timeout=30)
            self.assertEqual(stopped.returncode, 3)
            self.assertFalse(json.loads(stopped.stdout)['complete'])


    def report(self):
        return dict(rows=[dict(scenario=s, double_speed=speed, match=True,
            captures={core: dict(repeatable=True, observer_equivalent=True) for core in ('gbb', 'sameboy')})
            for s in probe.SCENARIOS for speed in (False, True)])

    def test_matrix_requires_all_cases_and_observer_equivalence(self):
        report = self.report()
        self.assertTrue(probe.passes(report))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1])))
        self.assertFalse(probe.passes(dict(report, rows=report['rows'][:-1] + [report['rows'][0]])))
        for field in ('repeatable', 'observer_equivalent'):
            changed = copy.deepcopy(report)
            changed['rows'][0]['captures']['sameboy'][field] = False
            self.assertFalse(probe.passes(changed))
        changed = copy.deepcopy(report)
        changed['rows'][0]['double_speed'] = 0
        self.assertFalse(probe.passes(changed))

    def test_cli_keeps_mismatch_and_failure_exit_codes(self):
        report = self.report()
        report['rows'][0]['match'] = False
        argv = ['probe', '--gbb-clock-trace', '/unused', '--sameboy-clock-trace', '/unused', '--output', '/unused']
        with patch.object(probe, 'run_probe', return_value=report), patch.object(sys, 'argv', argv), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)
        with patch.object(probe, 'run_probe', side_effect=ValueError('bad trace')), patch.object(sys, 'argv', argv), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            probe.main()
        self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
