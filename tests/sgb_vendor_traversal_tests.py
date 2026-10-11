#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded authored traversal observations and native playback controls."""
import argparse
import copy
import json
import contextlib
import io
from unittest.mock import patch
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import sgb_vendor_directory_tests as directory
from build_sgb_vendor_handoff_fixture import build, PROFILES, STATES
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_traversal_reference import summarize
import check_sgb_vendor_handoff_reference as reference

TIMELINE = None


def synthetic():
    data=directory.synthetic()
    data['selection_traversal_reads']=[]
    for event in data['events']:
        if event['register']!=0x4C: continue
        request=event['request'];slot=data['music'][request-1]-1
        for offset,address in enumerate((0x2B10+4*slot,0x2B11+4*slot,
                0x2B24+16*slot,0x2B25+16*slot,0x2B60+48*slot,0x2B6C+48*slot)):
            data['selection_traversal_reads'].append(dict(request=request,half=event['half']-18+offset,address=address))
    return data


class TraversalContracts(unittest.TestCase):
    def test_summary_and_guards(self):
        data=synthetic();result=summarize(data,'base','active')
        self.assertEqual(len(result),3)
        self.assertEqual(result[0]['pitch_setup_after_track_read_ms'],3/2048)
        self.assertEqual(result[0]['traversal_stages']['root']['after_previous_stage_ms'],1/2048)
        repeated=copy.deepcopy(data)
        repeated['selection_traversal_reads'].insert(0,dict(request=1,half=120,address=0x2B10))
        self.assertEqual(summarize(repeated,'base','active')[0]['traversal_stages']['root']['reads'],3)
        for fault in ('missing','empty','bound','extra','bool','request','address','early','late','order',
                      'root-byte','pattern-byte','note-byte','wrong-song','before-directory','after-setup'):
            with self.subTest(fault=fault):
                changed=copy.deepcopy(data);reads=changed['selection_traversal_reads'];r=reads[0]
                if fault=='missing': changed.pop('selection_traversal_reads')
                elif fault=='empty': reads.clear()
                elif fault=='bound': reads*=29
                elif fault=='extra': r['value']=0
                elif fault=='bool': r['half']=True
                elif fault=='request': r['request']=0
                elif fault=='address': r['address']=0x2B06
                elif fault=='early': r['half']=99
                elif fault=='late': r['half']=1000
                elif fault=='order': reads.reverse()
                elif fault=='root-byte': reads.pop(0)
                elif fault=='pattern-byte': reads.pop(2)
                elif fault=='note-byte': reads.pop(5)
                elif fault=='wrong-song': r['address']=0x2B14
                elif fault=='before-directory': r['half']=120
                elif fault=='after-setup': reads[5]['half']=995
                with self.assertRaises(ValueError): summarize(changed,'base','active')

    def test_existing_report_modes(self):
        # Exercise the shared CLI with authored metadata only, without originals
        # or a subprocess; older modes must retain their flag and report schema.
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'sgb2.program.rom').write_bytes(b'owned test identity')
            arguments=['check','--probe',str(root/'probe'),'--firmware-dir',str(root),
                       '--model','sgb2','--profile','base','--state','active']
            for mode in ('handoff','directory','traversal'):
                with self.subTest(mode=mode), patch.object(sys,'argv',arguments), \
                        patch.object(reference,'capture',return_value=synthetic()) as capture, \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    reference.main(directory=mode=='directory',traversal=mode=='traversal')
                    report=json.loads(output.getvalue())
                    self.assertEqual(report['schema'],'gbb-sgb-vendor-'+mode+'-v1')
                    self.assertIs(report['qualification'],False)
                    self.assertEqual(capture.call_count,2)
                    flags=capture.call_args.kwargs
                    self.assertEqual(flags,dict(selection_handoff=mode=='handoff',
                        selection_directory=mode=='directory',selection_traversal=mode=='traversal'))
                    item=report['runs'][0]['cases']['base-active']['original'][0]
                    self.assertEqual('directory_reads' in item,mode!='handoff')
                    self.assertEqual('traversal_reads' in item,mode=='traversal')

    def test_physical_traversal(self):
        if TIMELINE is None: self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); rom=root/'owned.rom'; rom.write_bytes(firmware())
            inputs=root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            for model in ('sgb','sgb2'):
                for profile in PROFILES:
                    for state in STATES:
                        with self.subTest(model=model,profile=profile,state=state):
                            game=root/'game.gb'; game.write_bytes(build(profile,state))
                            command=[str(TIMELINE),str(rom),str(game),model,'200000000',str(inputs),'fixture']
                            child=subprocess.run(command+['--selection-traversal'],capture_output=True,timeout=120)
                            self.assertEqual(child.returncode,0,child.stderr)
                            self.assertLessEqual(len(child.stdout),65536)
                            data=json.loads(child.stdout)
                            self.assertEqual(len(summarize(data,profile,state)),3)
                            for key in ('selection_traversal_reads','selection_directory_reads','handoff_writes','selection_pitch_writes',
                                        'spc_reply_handshakes','port_handshakes'):
                                data.pop(key)
                            baseline=subprocess.run(command,capture_output=True,timeout=120)
                            self.assertEqual(baseline.returncode,0,baseline.stderr)
                            self.assertEqual(data,json.loads(baseline.stdout))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--timeline-probe',type=Path)
    args,rest=parser.parse_known_args()
    TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
