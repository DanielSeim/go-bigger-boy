#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Controlled SOUND-field attribution, pitch setup and owned replay guards."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_vendor_reply_tests as reply
from build_sgb_vendor_handoff_fixture import build, CONTROLS, PROFILES, STATES
from build_sgb_vendor_dispatch_fixture import build as dispatch
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_handoff_reference import summarize

PROBE = TIMELINE = None


def synthetic():
    data = reply.synthetic()
    data['handoff_writes'] = [dict(request=h['request'],port=h['port'],half=h['half'],
        fields=1 if h['port']==0 else 14,first_music_reply=r['first_equal_reply'] if h['port']==0 else 0,
        music_reply_writes=r['equal_reply_writes'] if h['port']==0 else 0)
        for h,r in zip(data['port_handshakes'],data['spc_reply_handshakes'])]
    data['selection_pitch_writes'] = [dict(request=e['request'],half=e['half']-10,pitch=e['pitch'])
                                     for e in data['events'] if e['register']==0x4C]
    return data


class HandoffContracts(unittest.TestCase):
    def test_fixtures_and_export(self):
        images = [build(p,s) for p in PROFILES for s in STATES]
        self.assertEqual(len({hashlib.sha256(i).digest() for i in images}),8)
        for state,identity in (('active','6e6a0237612e6afc13c9361d0f87e8bd1bd7ea4d9af9e89f9992d2d3d669a949'),
                               ('completed','76555b58ed914b2a07e4a1fde53f5585b3eddc4f063a9894150269fbdabdaedf')):
            self.assertEqual(build('base',state),dispatch('change',state))
            self.assertEqual(hashlib.sha256(build('base',state)).hexdigest(),identity)
        for profile in PROFILES:
            for state in STATES:
                self.assertEqual(build(profile,state),build(profile,state))
                self.assertEqual(len(build(profile,state)),32768)
        for profile,state in (('bad','active'),('base','bad')):
            with self.assertRaises(ValueError): build(profile,state)
        for controls in ((0,), (1,0), (False,0), [0,0]):
            with self.assertRaises(ValueError): dispatch(effects=controls)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'game.gb'
            command = [sys.executable,str(Path(__file__).resolve().parents[1]/
                       'scripts/build_sgb_vendor_handoff_fixture.py'),'--profile','stop-both','--output',str(output)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(output.read_bytes(),build('stop-both'))
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(output.read_bytes(),build('stop-both'))

    def test_handoff_summary_and_guards(self):
        data = synthetic(); result = summarize(data,'base','active')
        self.assertEqual(result[0]['pitch_setup_ms'],890/2048)
        self.assertEqual(result[0]['keyon_after_pitch_setup_ms'],10/2048)
        self.assertEqual(result[0]['last_music_value_write_before_setup']['port'],0)
        no_candidate = copy.deepcopy(data)
        for w in no_candidate['handoff_writes']:
            if w['fields']==1:
                w.update(fields=0,music_reply_writes=0,first_music_reply=0)
        self.assertIsNone(summarize(no_candidate,'base','active')[0]['last_music_value_write_before_setup'])
        for fault in ('missing','length','extra','type','request','port','half','mask',
                      'count','music-identity','time','no-writes','disjoint','pitch-missing',
                      'pitch-extra','pitch-type','pitch-request','pitch-early','pitch-late',
                      'pitch-order','pitch-word','pitch-bound'):
            with self.subTest(fault=fault):
                changed = copy.deepcopy(data); record = changed['handoff_writes'][0]
                if fault == 'missing': changed.pop('handoff_writes')
                elif fault == 'length': changed['handoff_writes'].pop()
                elif fault == 'extra': record['value']=1
                elif fault == 'type': record['fields']=True
                elif fault == 'request': record['request']=2
                elif fault == 'port': record['port']=1
                elif fault == 'half': record['half']+=1
                elif fault == 'mask': record['fields']=2
                elif fault == 'count': record['music_reply_writes']=3
                elif fault == 'music-identity': record['music_reply_writes']=1
                elif fault == 'time': record['first_music_reply']=114
                elif fault == 'no-writes': record.update(fields=0,music_reply_writes=0)
                elif fault == 'disjoint': changed['handoff_writes'][1].update(music_reply_writes=1,first_music_reply=115)
                elif fault == 'pitch-missing': changed.pop('selection_pitch_writes')
                elif fault == 'pitch-extra': changed['selection_pitch_writes'][0]['value']=1
                elif fault == 'pitch-type': changed['selection_pitch_writes'][0]['half']=True
                elif fault == 'pitch-request': changed['selection_pitch_writes'][0]['request']=4
                elif fault == 'pitch-early': changed['selection_pitch_writes'][0]['half']=99
                elif fault == 'pitch-late': changed['selection_pitch_writes'][0]['half']=1000
                elif fault == 'pitch-order': changed['selection_pitch_writes'].reverse()
                elif fault == 'pitch-word': changed['selection_pitch_writes'][0]['pitch']=1357
                elif fault == 'pitch-bound': changed['selection_pitch_writes']*=43
                with self.assertRaises(ValueError): summarize(changed,'base','active')

    def test_physical_handoff_and_replay(self):
        if PROBE is None or TIMELINE is None: self.skipTest('playback probes not supplied')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); rom=root/'owned.rom'; rom.write_bytes(firmware())
            inputs=root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for profile in PROFILES:
                    for state in STATES:
                        with self.subTest(model=model,profile=profile,state=state):
                            game=root/'game.gb'; game.write_bytes(build(profile,state))
                            command=[str(TIMELINE),str(rom),str(game),model,'200000000',str(inputs),'fixture']
                            child=subprocess.run(command+['--selection-handoff'],capture_output=True,timeout=120)
                            self.assertEqual(child.returncode,0,child.stderr)
                            data=json.loads(child.stdout)
                            self.assertEqual(len(summarize(data,profile,state)),3)
                            for key in ('handoff_writes','selection_pitch_writes','spc_reply_handshakes','port_handshakes'):
                                data.pop(key)
                            baseline=subprocess.run(command,capture_output=True,timeout=120)
                            self.assertEqual(baseline.returncode,0,baseline.stderr)
                            self.assertEqual(data,json.loads(baseline.stdout))
                            if profile=='stop-both':
                                child=subprocess.run([str(PROBE),str(rom),str(game),model,
                                    '200000000','native','fixture'],capture_output=True,timeout=180)
                                self.assertEqual(child.returncode,0,child.stderr)
                                report=json.loads(child.stdout)
                                self.assertIs(report['reset_equal'],True); self.assertIs(report['restore_equal'],True)
                                self.assertEqual((report['starts'],report['completes'],report['notes'],report['rejected']),
                                                 (3,1 if state=='active' else 3,3,0))
                                self.assertGreater(report['nonzero'],100)


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--timeline-probe',type=Path)
    args,rest=parser.parse_known_args()
    PROBE=args.probe.resolve() if args.probe else None
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
