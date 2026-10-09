#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical upload rejection, silence and fresh-generation retry without reset."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_score_atomic_tests as atomic

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_sgb_score_transport import build as program
from build_sgb_score_recovery_fixture import build as cartridge, FAULTS
from build_sgb_score_atomic_fixture import build as control_cartridge
PROBE = None
OPTIONS = {**atomic.OPTIONS,'atomic_upload':True}


class RecoveryTests(unittest.TestCase):
    def probe(self, model, kind='asset-gap', clocks=100000000, mode='native',
              recovery=True, control=False, **fixture):
        if PROBE is None:
            self.skipTest('whole-host probe required')
        with tempfile.TemporaryDirectory() as directory:
            rom,game = Path(directory)/'host.rom',Path(directory)/'game.gb'
            rom.write_bytes(program(**OPTIONS,upload_recovery=recovery))
            game.write_bytes(control_cartridge('interleave',active=True) if control else cartridge(kind,**fixture))
            child = subprocess.run([str(PROBE),str(rom),str(game),model,str(clocks),mode],
                                   capture_output=True,text=True,timeout=90)
            self.assertEqual(child.returncode,0,child.stderr)
            self.assertLess(len(child.stdout),4096)
            result = json.loads(child.stdout)
            self.assertEqual(result['schema'],'gbb-score-transport-v1')
            self.assertIs(result['qualification'],False)
            self.assertIs(result['playback'],False)
            self.assertIs(result['reset_equal'],True)
            self.assertIs(result['restore_equal'],True)
            self.assertLessEqual(result['restore_count'],4096)
            self.assertLessEqual(result['pcm']['frames'],250000)
            self.assertLessEqual(result['clocks']-clocks,256)
            return result

    def evidence(self, result, attempts, events, suppressed, blocked=0):
        mask = (1 << events)-1
        self.assertEqual(result['recovery_rejections'],attempts)
        self.assertEqual(result['suppressed_sound'],suppressed)
        self.assertEqual(result['recovery_blocked'],blocked)
        self.assertEqual(result['clear_events'],events)
        self.assertEqual(result['clear_verified'],mask)
        self.assertEqual(result['clear_invalidated'],mask)
        self.assertEqual(result['restore_rejections'],attempts)
        self.assertEqual(result['restore_suppressed'],suppressed)
        self.assertEqual(result['restore_clears'],mask)
        self.assertEqual(result['blocked_nonzero'],0)
        if attempts:
            self.assertGreater(result['blocked_frames'],1000)
        self.assertIn(result['host_stack'],(0x1FF,0x1FD))

    def ready(self, result, kind, cold=False, repeat=False, stop=False, mixed=False):
        attempts = 2 if repeat or mixed else 1
        initial = 0 if cold else 1
        bad_transfers = 1 if mixed else attempts if kind == 'bad-content' else 0
        self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                          result['version'],result['bridge'],result['signature'],
                          result['error'],result['external'],result['selected_song'],
                          result['admitted_roots'],result['sounds']),
                         (1,initial+bad_transfers+1,initial+2,0xD5,2,0xA5,0,0,1 if cold else 3,3,2))
        self.assertEqual(result['score_tick'],72 if cold else 76)
        self.assertEqual(result['atomic_phase'],0xA5)
        self.assertEqual(result['restore_roots'],7)
        self.assertEqual(result['voice_env'],[0,0])
        self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)
        self.assertGreater(result['pcm']['nonzero_frames'],100)
        self.evidence(result,attempts,initial+attempts+1,attempts+int(stop))
        atomic.AtomicTests.regions(self,result,True)

    def test_caps_previous_image_and_reproducible_exports(self):
        self.assertEqual(hashlib.sha256(program(**OPTIONS)).hexdigest(),
                         'f915a07c9276f5ba269871b4e0ae5e699afb20cf18256dd6b58bb3766f25139f')
        image = program(**OPTIONS,upload_recovery=True)
        self.assertEqual(len(image),262144)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         'bcbe4765596df4db0232bb23526c804bc9f7812a796cd16b2457cf697769a357')
        for flags in ({'upload_recovery':True},{**OPTIONS,'upload_recovery':1}):
            with self.assertRaises(ValueError):
                program(**flags)
        for flags in ({'kind':'unknown'},{'final':'unknown'},{'repeat':1},
                      {'mixed':True},{'mixed':True,'cold':True,'stop':True},
                      {'invalid_root':1},{'kind':'bad-content','invalid_root':True},
                      {'kind':'bad-content','invalid_root':4},{'semantic_tail':True},
                      {'kind':'bad-content','semantic_tail':1}):
            with self.assertRaises(ValueError):
                cartridge(**flags)
        with tempfile.TemporaryDirectory() as directory:
            for index,(script,flags,expected) in enumerate((
                    ('build_sgb_score_transport.py',['--'+k.replace('_','-') for k in (*OPTIONS,'upload_recovery')],image),
                    ('build_sgb_score_recovery_fixture.py',['--repeat','--stop'],cartridge(repeat=True,stop=True)),
                    ('build_sgb_score_recovery_fixture.py',['--kind','bad-content','--invalid-root','3','--no-recover'],
                     cartridge('bad-content',invalid_root=3,recover=False)),
                    ('build_sgb_score_recovery_fixture.py',['--kind','bad-content','--repeat','--semantic-tail'],
                     cartridge('bad-content',repeat=True,semantic_tail=True)))):
                path = Path(directory)/f'{index}.bin'
                cmd = [sys.executable,str(ROOT/'scripts'/script),*flags,'--output',str(path)]
                child = subprocess.run(cmd,capture_output=True,text=True,timeout=10)
                self.assertEqual(child.returncode,0,child.stderr)
                self.assertEqual(path.read_bytes(),expected)
                self.assertNotEqual(subprocess.run(cmd,capture_output=True,timeout=10).returncode,0)
                self.assertEqual(path.read_bytes(),expected)

    def test_all_prior_rejections_allow_fresh_complete_generation(self):
        for model in ('sgb','sgb2'):
            for kind in FAULTS:
                with self.subTest(model=model,kind=kind):
                    result = self.probe(model,kind)
                    self.ready(result,kind)
                    self.assertEqual(result['interruptions'],4 if kind == 'bad-content' else 1)
                    self.assertEqual(result['active_env'],result['interruptions'])
                    self.assertGreater(result['interrupt_tick'],0)
                    self.assertLess(result['interrupt_tick'],32)

    def test_preflight_and_semantic_retry_in_all_render_modes(self):
        for model in ('sgb','sgb2'):
            for kind in ('asset-gap','bad-content'):
                baseline = None
                for mode in ('native','scalar','combined'):
                    result = self.probe(model,kind,mode=mode,final='split')
                    self.ready(result,kind)
                    if baseline is None:
                        baseline = result
                    elif mode == 'scalar':
                        self.assertEqual(result['pcm'],baseline['pcm'])
                    else:
                        self.assertLessEqual(abs(result['pcm']['frames']-baseline['pcm']['frames']*3//2),2)

    def test_repeated_failures_and_stop_do_not_leak_stack_or_readiness(self):
        for model in ('sgb','sgb2'):
            for kind in ('asset-gap','bad-content'):
                for mode in ('native','scalar','combined'):
                    result = self.probe(model,kind,repeat=True,stop=True,mode=mode,final='reverse')
                    self.ready(result,kind,repeat=True,stop=True)

    def test_cold_recovery(self):
        for model in ('sgb','sgb2'):
            for kind in ('missing-score','missing-assets','asset-gap','bad-content'):
                result = self.probe(model,kind,cold=True)
                self.ready(result,kind,cold=True)
                self.assertEqual(result['natural_ends'],4)

    def test_mixed_failure_orders_recover_in_all_render_modes(self):
        for model in ('sgb','sgb2'):
            for kind in ('asset-gap','bad-content'):
                for mode in ('native','scalar','combined'):
                    result = self.probe(model,kind,cold=True,mixed=True,mode=mode)
                    self.ready(result,kind,cold=True,mixed=True)

    def test_sounds_and_stops_remain_muted_without_valid_retry(self):
        for model in ('sgb','sgb2'):
            for kind in ('asset-gap','bad-content'):
                for stop in (False,True):
                    result = self.probe(model,kind,recover=False,stop=stop,clocks=45000000)
                    self.assertEqual((result['status'],result['version'],result['bridge'],
                                      result['signature'],result['external'],result['admitted_roots'],
                                      result['selected_song'],result['error'],result['transfers'],
                                      result['adoptions']),
                                     (9,0xD5,0xE2,0xA5,0,0,0,2 if kind == 'bad-content' else 1,
                                      2 if kind == 'bad-content' else 1,2))
                    self.evidence(result,1,2,1+int(stop),blocked=1)
                    self.assertEqual(result['atomic_phase'],0xA4)
                    self.assertEqual(result['voice_env'],[0,0])
                    self.assertGreater(result['clocks']-result['last_nonzero_clock'],1000000)
                    if kind == 'bad-content':
                        atomic.AtomicTests.regions(self,result,True,bad=True)
                    else:
                        self.assertEqual(result['score_hash'],atomic.fnv(bytes(2048)))
                        self.assertEqual(result['asset_hash'],atomic.fnv(bytes(192)))

    def test_previous_profile_halts_before_later_retry(self):
        for model in ('sgb','sgb2'):
            for kind in ('asset-gap','bad-content'):
                result = self.probe(model,kind,recovery=False)
                self.assertEqual((result['status'],result['transfers'],result['adoptions']),
                                 (255,2 if kind == 'bad-content' else 1,2))
                self.assertEqual(result['recovery_rejections'],0)
                if kind == 'bad-content':
                    atomic.AtomicTests.regions(self,result,True,bad=True)
                else:
                    atomic.AtomicTests.regions(self,result,False)

    def test_partial_directory_admission_is_revoked_before_recovery(self):
        for model in ('sgb','sgb2'):
            for root in (1,2,3):
                with self.subTest(model=model,root=root):
                    result = self.probe(model,'bad-content',invalid_root=root)
                    self.ready(result,'bad-content')
                    result = self.probe(model,'bad-content',invalid_root=root,
                                        recover=False,clocks=45000000)
                    self.assertEqual((result['status'],result['bridge'],result['error'],
                                      result['admitted_roots'],result['selected_song'],
                                      result['transfers'],result['adoptions']),
                                     (9,0xE2,2,0,0,2,2))
                    self.evidence(result,1,2,1,blocked=1)
                    self.assertEqual(result['atomic_phase'],0xA4)
                    score = bytearray(atomic.bank('inherit'))
                    score[2*(root-1):2*root] = bytes(2)
                    self.assertEqual(result['score_hash'],atomic.fnv(score))
                    data = atomic.assets('mixed2',profile='distinct',
                                         brr_profile='swap',layout='last')
                    self.assertEqual(result['asset_hash'],atomic.fnv(data))
                    self.assertEqual(result['voice_env'],[0,0])

    def test_short_final_chunk_cannot_alias_repeated_loader_requests(self):
        for model in ('sgb','sgb2'):
            for mode in ('native','scalar','combined'):
                result = self.probe(model,'bad-content',repeat=True,
                                    cold=True,semantic_tail=True,mode=mode)
                self.ready(result,'bad-content',cold=True,repeat=True)

    def test_complete_transactions_without_failures(self):
        for model in ('sgb','sgb2'):
            result = self.probe(model,control=True,clocks=75000000)
            self.assertEqual((result['status'],result['transfers'],result['adoptions'],
                              result['version'],result['bridge'],result['admitted_roots']),
                             (1,2,3,0xD5,2,3))
            self.evidence(result,0,2,0)
            self.assertEqual(result['blocked_frames'],0)
            atomic.AtomicTests.regions(self,result,True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    args,remaining = parser.parse_known_args()
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0],*remaining])
