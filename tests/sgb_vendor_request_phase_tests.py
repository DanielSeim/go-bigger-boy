#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame-phase fixtures, bounded traversal summaries and native playback controls."""
import argparse
import copy
import contextlib
import io
from unittest.mock import patch
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_vendor_traversal_tests as traversal
from build_sgb_vendor_request_phase_fixture import build, request_frames, SCHEDULES, PROFILES, STATES
from build_sgb_vendor_handoff_fixture import build as handoff
from build_sgb_vendor_dispatch_fixture import build as dispatch
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_request_phase_reference import summarize, compare
import check_sgb_vendor_request_phase_reference as reference

TIMELINE=None


class RequestPhaseContracts(unittest.TestCase):
    def test_fixtures_and_export(self):
        identities=set()
        for schedule in SCHEDULES:
            for profile in PROFILES:
                for state in STATES:
                    image=build(schedule,profile,state)
                    self.assertEqual(image,build(schedule,profile,state))
                    self.assertEqual(len(image),32768)
                    identities.add(hashlib.sha256(image).digest())
                    baseline=handoff(profile,state)
                    self.assertEqual(image[0x4000:0x6000],baseline[0x4000:0x6000])
                    if schedule=='baseline': self.assertEqual(image,baseline)
                    frames=request_frames(schedule,state)
                    self.assertEqual(frames[1],frames[2])
                    if schedule.startswith('first-'):
                        self.assertEqual(frames,(16+int(schedule[-1]),16 if state=='active' else 100,
                                                16 if state=='active' else 100))
                    if schedule.startswith('spacing-'):
                        self.assertEqual(frames,(16,(16 if state=='active' else 100)+int(schedule[-1]),
                                                (16 if state=='active' else 100)+int(schedule[-1])))
        self.assertEqual(len(identities),20)
        for schedule,profile,state in (('first-3','base','active'),('baseline','stop-a','active'),
                                       ('baseline','base','bad')):
            with self.assertRaises(ValueError): build(schedule,profile,state)
        for frames in ([16,16,16],(16,16),(False,16,16),(0,16,16),(105,16,16)):
            with self.assertRaises(ValueError): dispatch(request_frames=frames)
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary)/'game.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/
                'scripts/build_sgb_vendor_request_phase_fixture.py'),'--schedule','spacing-2',
                '--state','completed','--profile','stop-both','--output',str(output)]
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(output.read_bytes(),build('spacing-2','stop-both','completed'))
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,2)
            self.assertEqual(output.read_bytes(),build('spacing-2','stop-both','completed'))

    def test_phase_summary_and_guards(self):
        data=traversal.synthetic()
        baseline=summarize(data,'baseline','base','active')
        self.assertEqual(baseline['request_frames'],[16,16,16])
        self.assertEqual(len(baseline['request_gaps_ms']),2)
        measured=summarize(data,'spacing-1','base','active')
        self.assertEqual(measured['request_frames'],[16,17,17])
        self.assertTrue(all(x['directory_to_root_delta_ms']==0 for x in compare(baseline,measured)))
        changed=copy.deepcopy(measured)
        changed['selections'][1]['selection_ms']+=2
        changed['selections'][1]['traversal_stages']['root']['after_previous_stage_ms']+=1
        self.assertEqual(compare(baseline,changed)[1]['selection_delta_ms'],2)
        self.assertEqual(compare(baseline,changed)[1]['directory_to_root_delta_ms'],1)
        for fault in ('note','root','window'):
            bad=copy.deepcopy(data)
            if fault=='note': bad['events'][0]['pitch']=1
            elif fault=='root': bad['selection_traversal_reads'].pop(0)
            else: bad['selection_traversal_reads'][0]['address']=0x2B06
            with self.assertRaises(ValueError): summarize(bad,'baseline','base','active')
        for schedule,profile,state in (('bad','base','active'),('baseline','stop-a','active'),('baseline','base','bad')):
            with self.assertRaises(ValueError): summarize(data,schedule,profile,state)
        for fault in ('baseline','length','music'):
            bad=copy.deepcopy(measured)
            if fault=='baseline': bad['request_frames']=[17,16,16]
            elif fault=='length': bad['selections'].pop()
            else: bad['selections'][0]['music']=3
            with self.assertRaises(ValueError): compare(bad,measured)

    def test_filtered_reference_keeps_baseline(self):
        identities={hashlib.sha256(build(schedule,profile,state)).digest():(profile,state)
            for schedule in SCHEDULES for profile in PROFILES for state in STATES}
        def capture(probe,program,game,model,clocks,inputs,**flags):
            self.assertEqual(flags,dict(selection_traversal=True))
            self.assertEqual(clocks,260000000)
            profile,state=identities[hashlib.sha256(game.read_bytes()).digest()]
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
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for filename in ('sgb1.program.rom','sgb2.program.rom'):
                (root/filename).write_bytes(b'owned test identity')
            arguments=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),
                       '--schedule','first-2','--schedule','first-2']
            with patch.object(sys,'argv',arguments), patch.object(reference,'capture',side_effect=capture) as mocked, \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                reference.main()
            report=json.loads(output.getvalue())
            self.assertEqual(report['schema'],'gbb-sgb-vendor-request-phase-v1')
            self.assertIs(report['qualification'],False)
            self.assertEqual(mocked.call_count,32)
            self.assertEqual([r['model'] for r in report['runs']],['sgb','sgb2'])
            for run in report['runs']:
                self.assertEqual(len(run['cases']),8)
                for profile in PROFILES:
                    for state in STATES:
                        self.assertIn('baseline-'+profile+'-'+state,run['cases'])
                        self.assertIn('first-2-'+profile+'-'+state,run['cases'])

    def test_physical_request_phase(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);rom=root/'owned.rom';rom.write_bytes(firmware())
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for schedule in SCHEDULES:
                    for profile in PROFILES:
                        for state in STATES:
                            with self.subTest(model=model,schedule=schedule,profile=profile,state=state):
                                game=root/'game.gb';game.write_bytes(build(schedule,profile,state))
                                command=[str(TIMELINE),str(rom),str(game),model,'200000000',str(inputs),'fixture']
                                child=subprocess.run(command+['--selection-traversal'],capture_output=True,timeout=120)
                                self.assertEqual(child.returncode,0,child.stderr)
                                self.assertLessEqual(len(child.stdout),65536)
                                data=json.loads(child.stdout)
                                report=summarize(data,schedule,profile,state)
                                self.assertEqual(len(report['selections']),3)
                                for key in ('selection_traversal_reads','selection_directory_reads','handoff_writes',
                                            'selection_pitch_writes','spc_reply_handshakes','port_handshakes'):
                                    data.pop(key)
                                baseline=subprocess.run(command,capture_output=True,timeout=120)
                                self.assertEqual(baseline.returncode,0,baseline.stderr)
                                self.assertEqual(data,json.loads(baseline.stdout))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path)
    args,rest=parser.parse_known_args()
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
