#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Atomic owned score/sample uploads through physical GB VRAM and IPL."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from build_sgb_score_transport import build as program
from build_sgb_score_atomic_fixture import build as cartridge, payload, VALID, FAULTS
from build_sgb_score_relocated_fixture import assets, bank
PROBE = None
OPTIONS = dict(multisong=True, uploaded_instrument=True, two_instruments=True,
               multiblock=True, instrument_profiles=True, one_shot=True,
               brr_profiles=True, relocatable=True)


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte)*1099511628211) & ((1 << 64)-1)
    return value


class AtomicTests(unittest.TestCase):
    def probe(self, model, kind='full', clocks=75000000, mode='native', atomic=True, **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom, game = Path(directory)/'host.rom', Path(directory)/'game.gb'
            rom.write_bytes(program(**OPTIONS, atomic_upload=atomic))
            game.write_bytes(cartridge(kind, **fixture))
            child = subprocess.run([str(PROBE), str(rom), str(game), model, str(clocks), mode],
                                   capture_output=True, text=True, timeout=90)
            self.assertEqual(child.returncode, 0, child.stderr)
            self.assertLess(len(child.stdout), 4096)
            result = json.loads(child.stdout)
            self.assertEqual(result['schema'], 'gbb-score-transport-v1')
            self.assertIs(result['qualification'], False)
            self.assertIs(result['playback'], False)
            self.assertIs(result['reset_equal'], True)
            self.assertIs(result['restore_equal'], True)
            self.assertLessEqual(result['restore_count'], 4096)
            self.assertLessEqual(result['pcm']['frames'], 250000)
            self.assertLessEqual(result['clocks']-clocks, 256)
            return result

    def cleared(self, result, mask, phase=0xA5):
        self.assertEqual(result['atomic_clears'], mask)
        self.assertEqual(result['atomic_invalidated'], mask)
        self.assertEqual(result['atomic_phase'], phase)

    def regions(self, result, replacement, bad=False):
        score = bank('inherit' if replacement else 'all2')
        data = bytearray(assets('mixed2' if replacement else 'mixed3',
                               profile='distinct' if replacement else 'baseline',
                               brr_profile='swap' if replacement else 'mixed',
                               layout='last' if replacement else 'near'))
        if bad:
            data[0] = 4
        self.assertEqual(result['score_hash'], fnv(score))
        self.assertEqual(result['asset_hash'], fnv(data))
        self.assertEqual(result['sample_starts'], [int.from_bytes(data[e:e+2], 'little') for e in (8,12)])
        self.assertEqual(result['sample_loops'], [int.from_bytes(data[e:e+2], 'little') for e in (10,14)])
        self.assertEqual(result['sample_modes'], [data[19],data[23]])

    def ready(self, result, song=3, transfers=2):
        self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                          result['version'],result['bridge'],result['signature'],
                          result['sounds'],result['error'],result['external'],
                          result['selected_song'],result['admitted_roots']),
                         (1,transfers,transfers+1,0xD4,2,0xA5,2,0,0,song,3))
        self.assertEqual(result['restore_roots'],7)
        self.assertEqual(result['score_tick'],72 if song != 3 else 76)
        self.assertEqual(result['voice_env'],[0,0])
        self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)

    def test_caps_prior_images_fixtures_and_reproducible_cli(self):
        self.assertEqual(hashlib.sha256(program(**OPTIONS)).hexdigest(),
                         '771bf03cfc3c1c091488fea1c0d26b9f78847edb970c9a7a44dab636d7618600')
        image = program(**OPTIONS, atomic_upload=True)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         'f915a07c9276f5ba269871b4e0ae5e699afb20cf18256dd6b58bb3766f25139f')
        for options in ({'atomic_upload':True},{**OPTIONS,'atomic_upload':1}):
            with self.assertRaises(ValueError):
                program(**options)
        for options in ({'kind':'unknown'},{'active':1},{'cold':True,'third':True}):
            with self.assertRaises(ValueError):
                cartridge(**options)
        # Independently reconstruct complete valid lists by destination address.
        for kind in VALID:
            data, offset, memory = payload(kind,replacement=True),0,{}
            while True:
                count = int.from_bytes(data[offset:offset+2],'little')
                start = int.from_bytes(data[offset+2:offset+4],'little')
                offset += 4
                if count == 0:
                    self.assertEqual(start,0x400)
                    break
                for address, byte in enumerate(data[offset:offset+count],start):
                    self.assertNotIn(address,memory)
                    memory[address] = byte
                offset += count
            self.assertEqual(set(memory),set(range(0x2b00,0x3300))|set(range(0x5000,0x50c0)))
            self.assertEqual(bytes(memory[a] for a in range(0x2b00,0x3300)),bank('inherit'))
            self.assertEqual(bytes(memory[a] for a in range(0x5000,0x50c0)),
                             assets('mixed2',profile='distinct',brr_profile='swap',layout='last'))
        with tempfile.TemporaryDirectory() as directory:
            for script, flags, expected in (
                    ('build_sgb_score_transport.py',['--'+k.replace('_','-') for k in (*OPTIONS,'atomic_upload')],image),
                    ('build_sgb_score_atomic_fixture.py',['--kind','split','--active'],cartridge('split',active=True))):
                path = Path(directory)/script
                command = [sys.executable,str(ROOT/'scripts'/script),*flags,'--output',str(path)]
                child = subprocess.run(command,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,0,child.stderr)
                self.assertEqual(path.read_bytes(),expected)
                self.assertNotEqual(subprocess.run(command,capture_output=True,timeout=10).returncode,0)
                self.assertEqual(path.read_bytes(),expected)

    def test_previous_profile_exposes_stale_asset_reuse(self):
        # An owned black-box regression witness: D3 accepts a new score with
        # the old sample object when the replacement list omits that object.
        for model in ('sgb','sgb2'):
            result = self.probe(model,'missing-assets',active=True,atomic=False)
            self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                              result['version'],result['selected_song']), (1,2,3,0xD3,3))
            self.assertEqual(result['score_hash'],fnv(bank('inherit')))
            self.assertEqual(result['asset_hash'],fnv(assets('mixed3',layout='near')))
            self.cleared(result,0,phase=0)

    def test_cold_complete_objects_and_chunk_order(self):
        for model in ('sgb','sgb2'):
            for kind in VALID:
                with self.subTest(model=model,kind=kind):
                    result = self.probe(model,kind,cold=True,clocks=45000000)
                    self.ready(result,song=1,transfers=1)
                    self.cleared(result,1)
                    self.regions(result,True)
                    self.assertGreater(result['pcm']['nonzero_frames'],100)
                    self.assertEqual(result['natural_ends'],4) # source 2 one-shot on voice 3

    def test_changed_objects_active_replacement_in_all_render_modes(self):
        for model in ('sgb','sgb2'):
            for kind in VALID:
                baseline = None
                for mode in ('native','scalar','combined'):
                    with self.subTest(model=model,kind=kind,mode=mode):
                        result = self.probe(model,kind,active=True,mode=mode)
                        self.ready(result)
                        self.cleared(result,3)
                        self.regions(result,True)
                        self.assertEqual(result['interruptions'],4)
                        self.assertEqual(result['active_env'],4)
                        self.assertGreater(result['interrupt_tick'],0)
                        self.assertLess(result['interrupt_tick'],32)
                        self.assertEqual(result['restore_commands'],4)
                        self.assertEqual((result['source2'],result['source3']),(3,3))
                        self.assertEqual(result['voice_tuning'],[0,0])
                        if baseline is None:
                            baseline = result
                        elif mode == 'scalar':
                            self.assertEqual(result['pcm'],baseline['pcm'])
                        else:
                            self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2),2)

    def test_stop_and_third_generation(self):
        for model in ('sgb','sgb2'):
            for mode in ('native','scalar','combined'):
                result = self.probe(model,'split',active=True,stop=True,mode=mode)
                self.ready(result)
                self.cleared(result,3)
                self.regions(result,True)
                self.assertEqual(result['interruptions'],1)
                self.assertEqual(result['active_env'],1)
                self.assertEqual(result['restore_commands'],1)
            result = self.probe(model,'reverse',active=True,third=True,clocks=125000000)
            self.ready(result,song=2,transfers=3)
            self.cleared(result,7)
            self.regions(result,False)

    def test_incomplete_or_unsafe_replacement_never_releases_old_generation(self):
        for model in ('sgb','sgb2'):
            for kind in FAULTS:
                if kind == 'bad-content':
                    continue
                with self.subTest(model=model,kind=kind):
                    result = self.probe(model,kind,active=True,clocks=45000000)
                    self.assertEqual((result['status'],result['error'],result['transfers'],
                                      result['adoptions'],result['version'],result['bridge'],
                                      result['signature'],result['external'],result['admitted_roots']),
                                     (255,1,1,2,0xD4,1,0xA5,0,3))
                    self.cleared(result,1)
                    self.regions(result,False)
                    self.assertEqual(result['interruptions'],1)
                    self.assertEqual(result['active_env'],1)
                    self.assertEqual(result['voice_env'],[0,0])
                    self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)

    def test_incomplete_cold_upload_never_admits_assets(self):
        for model in ('sgb','sgb2'):
            for kind in ('missing-score','missing-assets','empty','asset-gap'):
                result = self.probe(model,kind,cold=True,clocks=30000000)
                self.assertEqual((result['status'],result['error'],result['transfers'],
                                  result['adoptions'],result['admitted_roots']), (255,1,0,1,0))
                self.cleared(result,0,phase=0)
                self.assertEqual(result['pcm']['nonzero_frames'],0)
                self.assertEqual(result['score_hash'],fnv(bytes(2048)))
                self.assertEqual(result['asset_hash'],fnv(bytes(192)))

    def test_complete_malformed_replacement_clears_old_data_and_stays_unpublished(self):
        for model in ('sgb','sgb2'):
            for mode in ('native','scalar','combined'):
                result = self.probe(model,'bad-content',active=True,mode=mode)
                self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                                  result['version'],result['bridge'],result['signature'],
                                  result['admitted_roots']), (255,2,2,0,0xE2,0,0))
                self.cleared(result,3,phase=0xA4)
                self.regions(result,True,bad=True)
                self.assertEqual(result['interruptions'],4)
                self.assertEqual(result['active_env'],4)
                self.assertEqual(result['voice_env'],[0,0])
                self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0],*remaining])
