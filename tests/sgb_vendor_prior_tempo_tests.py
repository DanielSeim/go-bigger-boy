#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prior-song tempo controls, held-out request spacing and native traversal guards."""
import argparse
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
import sgb_vendor_traversal_tests as traversal
from build_sgb_vendor_prior_tempo_fixture import build, request_frames, TEMPOS, SCHEDULES, PROFILES, STATES
from build_sgb_vendor_handoff_fixture import build as handoff
from build_sgb_vendor_dispatch_fixture import build as dispatch
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_prior_tempo_reference import summarize,compare
import check_sgb_vendor_prior_tempo_reference as reference

TIMELINE=PROBE=None


def synthetic(profile='base',state='active'):
    data=traversal.synthetic()
    if profile=='stop-both':
        for record in data['handoff_writes']:
            if record['fields']==14: record['fields']=8
    if state=='completed':
        notes=[e for e in data['events'] if e['register']==0x4C]
        data['events'] += [dict(request=e['request'],register=0x5C,value=255,half=e['half']+1)
                           for e in notes[:-1]]
        data['events'].sort(key=lambda e:e['half'])
    return data


class PriorTempoContracts(unittest.TestCase):
    def test_fixtures_and_export(self):
        identities=set()
        for schedule in SCHEDULES:
            for profile in PROFILES:
                for state in STATES:
                    baseline=build(45,schedule,profile,state);changed=build(96,schedule,profile,state)
                    for tempo,image in ((45,baseline),(96,changed)):
                        self.assertEqual(image,build(tempo,schedule,profile,state))
                        self.assertEqual(len(image),32768);identities.add(hashlib.sha256(image).digest())
                    differences={i for i,(a,b) in enumerate(zip(baseline,changed)) if a!=b}
                    self.assertIn(0x406D,differences)
                    self.assertTrue(differences<={0x14E,0x14F,0x406D})
                    self.assertEqual((baseline[0x406D],changed[0x406D]),(45,96))
                    self.assertEqual(baseline[0x5000:0x6000],changed[0x5000:0x6000])
                    if schedule=='baseline': self.assertEqual(baseline,handoff(profile,state))
                    self.assertEqual(request_frames(schedule,state),
                        (16,(16 if state=='active' else 100)+(3 if schedule=='spacing-3' else 0),
                            (16 if state=='active' else 100)+(3 if schedule=='spacing-3' else 0)))
        self.assertEqual(len(identities),16)
        for tempo,schedule,profile,state in ((True,'baseline','base','active'),(0,'baseline','base','active'),
            (96,'spacing-2','base','active'),(96,'baseline','stop-a','active'),(96,'baseline','base','bad')):
            with self.assertRaises(ValueError): build(tempo,schedule,profile,state)
        for tempos in ([45,45,45],(45,45),(False,45,45),(0,45,45),(256,45,45)):
            with self.assertRaises(ValueError): dispatch(track_tempos=tempos)
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary)/'game.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/
                'scripts/build_sgb_vendor_prior_tempo_fixture.py'),'--tempo','96','--schedule','spacing-3',
                '--profile','stop-both','--state','completed','--output',str(output)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(output.read_bytes(),build(96,'spacing-3','stop-both','completed'))
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(output.read_bytes(),build(96,'spacing-3','stop-both','completed'))

    def test_summary_and_comparison(self):
        for schedule in SCHEDULES:
            for profile in PROFILES:
                for state in STATES:
                    data=synthetic(profile,state)
                    baseline=summarize(data,45,schedule,profile,state)
                    changed=summarize(data,96,schedule,profile,state)
                    self.assertEqual(changed['track_tempos'],[96,45,45])
                    self.assertEqual(changed['authored_preceding_tempos'],[None,96,45])
                    self.assertEqual(len(changed['request_gaps_ms']),2)
                    self.assertTrue(all(x['directory_to_root_delta_ms']==0 for x in compare(baseline,changed)))
        data=synthetic();baseline=summarize(data,45,'baseline','base','active')
        changed=summarize(data,96,'baseline','base','active')
        changed['selections'][1]['traversal_stages']['root']['after_previous_stage_ms']-=1
        self.assertEqual(compare(baseline,changed)[1]['directory_to_root_delta_ms'],-1)
        for fault in ('baseline','frames','length','music'):
            bad=copy.deepcopy(baseline)
            if fault=='baseline': bad['track_tempos']=[96,45,45]
            elif fault=='frames': bad['request_frames']=[16,19,19]
            elif fault=='length': bad['selections'].pop()
            else: bad['selections'][0]['music']=3
            with self.assertRaises(ValueError): compare(bad,changed)
        for fault in ('note','root','window'):
            bad=copy.deepcopy(data)
            if fault=='note': bad['events'][0]['pitch']=1
            elif fault=='root': bad['selection_traversal_reads'].pop(0)
            else: bad['selection_traversal_reads'][0]['address']=0x2B06
            with self.assertRaises(ValueError): summarize(bad,96,'baseline','base','active')
        with self.assertRaises(ValueError): summarize(data,True,'baseline','base','active')

    def test_filtered_reference_keeps_tempo_baselines(self):
        identities={hashlib.sha256(build(t,s,p,state)).digest():(p,state)
            for t in TEMPOS for s in SCHEDULES for p in PROFILES for state in STATES}
        def capture(probe,program,game,model,clocks,inputs,**flags):
            self.assertEqual(flags,dict(selection_traversal=True));self.assertEqual(clocks,260000000)
            return synthetic(*identities[hashlib.sha256(game.read_bytes()).digest()])
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'sgb2.program.rom').write_bytes(b'owned test identity')
            arguments=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),
                       '--model','sgb2','--tempo','96','--tempo','96','--schedule','spacing-3']
            with patch.object(sys,'argv',arguments),patch.object(reference,'capture',side_effect=capture) as mocked, \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                reference.main()
            report=json.loads(output.getvalue())
            self.assertEqual(report['schema'],'gbb-sgb-vendor-prior-tempo-v1')
            self.assertIs(report['qualification'],False)
            self.assertEqual(mocked.call_count,16)
            cases=report['runs'][0]['cases'];self.assertEqual(len(cases),8)
            for profile in PROFILES:
                for state in STATES:
                    for tempo in TEMPOS: self.assertIn(f'{tempo}-spacing-3-{profile}-{state}',cases)

    def test_physical_prior_tempo(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);rom=root/'owned.rom';rom.write_bytes(firmware())
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for tempo in reversed(TEMPOS):
                    for schedule in SCHEDULES:
                        for profile in PROFILES:
                            for state in STATES:
                                with self.subTest(model=model,tempo=tempo,schedule=schedule,profile=profile,state=state):
                                    game=root/'game.gb';game.write_bytes(build(tempo,schedule,profile,state))
                                    command=[str(TIMELINE),str(rom),str(game),model,'200000000',str(inputs),'fixture']
                                    child=subprocess.run(command+['--selection-traversal'],capture_output=True,timeout=120)
                                    self.assertEqual(child.returncode,0,child.stderr)
                                    self.assertLessEqual(len(child.stdout),65536)
                                    data=json.loads(child.stdout)
                                    self.assertEqual(len(summarize(data,tempo,schedule,profile,state)['selections']),3)
                                    for key in ('selection_traversal_reads','selection_directory_reads','handoff_writes',
                                                'selection_pitch_writes','spc_reply_handshakes','port_handshakes'):
                                        data.pop(key)
                                    plain=subprocess.run(command,capture_output=True,timeout=120)
                                    self.assertEqual(plain.returncode,0,plain.stderr)
                                    self.assertEqual(data,json.loads(plain.stdout))


    def test_tempo_replay(self):
        if PROBE is None: self.skipTest('replay probe not supplied')
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);rom=root/'owned.rom';rom.write_bytes(firmware())
            for model in ('sgb','sgb2'):
                for state in STATES:
                    with self.subTest(model=model,state=state):
                        game=root/'game.gb';game.write_bytes(build(96,'spacing-3','stop-both',state))
                        child=subprocess.run([str(PROBE),str(rom),str(game),model,'200000000','native','fixture'],
                                             capture_output=True,timeout=180)
                        self.assertEqual(child.returncode,0,child.stderr)
                        report=json.loads(child.stdout)
                        self.assertIs(report['reset_equal'],True);self.assertIs(report['restore_equal'],True)
                        self.assertGreater(report['restores'],0);self.assertGreater(report['unread_restores'],0)
                        self.assertEqual((report['starts'],report['completes'],report['notes'],report['rejected']),
                                         (3,1 if state=='active' else 3,3,0))
                        self.assertGreater(report['nonzero'],100)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path)
    parser.add_argument('--probe',type=Path)
    args,rest=parser.parse_known_args()
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
