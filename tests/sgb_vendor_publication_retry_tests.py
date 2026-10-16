#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Paired gate scope, frozen retry anchors, holdouts and owned lifecycle controls."""
import argparse
from concurrent.futures import ThreadPoolExecutor
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
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import check_sgb_vendor_publication_retry_reference as reference
from check_sgb_vendor_delayed_tempo_reference import cases as previous_cases, summarize
from build_sgb_vendor_music import build as firmware
from sgb_vendor_delayed_tempo_tests import synthetic

TIMELINE=PROBE=None


def original_timeline(live):
    data=synthetic(live,8)
    operand=data['tempo_operand_reads'][0]['half']
    directory=max(r['half'] for r in data['selection_directory_reads'] if r['request']==2)
    reply=directory+120
    for key in ('port_handshakes','spc_reply_handshakes','handoff_writes'):
        if live==32:
            data[key].append(copy.deepcopy(data[key][-1]))
            early=data[key][-2]
            if key=='port_handshakes':early.update(first_read=0,last_read=0,first_equal=0,last_equal=0,equal_reads=0,unequal_reads=0)
            elif key=='spc_reply_handshakes':early.update(first_reply=0,last_reply=0,first_equal_reply=0,last_equal_reply=0,reply_writes=0,equal_reply_writes=0)
            else:early.update(first_music_reply=0,music_reply_writes=0)
        last=data[key][-1]
        if live==32:last['half']=operand+20
        if key=='port_handshakes':last.update(first_read=reply+1,last_read=reply+1,first_equal=reply+1,last_equal=reply+1)
        elif key=='spc_reply_handshakes':last.update(first_reply=reply,last_reply=reply,first_equal_reply=reply,last_equal_reply=reply)
        else:last.update(first_music_reply=reply)
    return data


def summaries(live):
    return reference.observe(original_timeline(live),live),reference.observe(synthetic(live,8,position='before'),live)


def row(case):
    group,model,live,gap,spin=case;a,b=summaries(live)
    return dict(group=group,model=model,live=live,gap=gap,spin=spin,
                fixture_sha256=hashlib.sha256(reference.fixture(case)).hexdigest(),original=a,replacement=b)


def witnesses():
    return [(g,m,l,schedules[1][0]+(4 if m=='sgb' else 0),schedules[1][1])
            for g,schedules in reference.SCHEDULES.items() for m in ('sgb','sgb2') for l in (32,128)]


class PublicationRetryContracts(unittest.TestCase):
    def test_scope_and_holdouts(self):
        cases=reference.cases();self.assertEqual(len(cases),32);self.assertEqual(len(set(cases)),32)
        previous=set(previous_cases())
        for case in cases:
            group,model,live,gap,spin=case
            coordinate=(model,live,8,gap,spin)
            self.assertEqual(coordinate in previous,group=='baseline')
            baseline=reference.fixture(('baseline',model,live,11 if model=='sgb' else 7,2048))
            self.assertEqual(reference.fixture(case)[0x4000:],baseline[0x4000:])
        for bad in (('holdout','sgb2',True,8,256),('holdout','sgb2',32,8,False),('other','sgb2',32,8,256)):
            with self.assertRaises(ValueError):reference.fixture(bad)

    def test_retry_anchors_and_timing_are_separate(self):
        for live in (32,128):
            a,b=summaries(live)
            self.assertTrue(reference.anchor(a,live))
            comparison=reference.compare(a,b)
            self.assertTrue(comparison['wait_within_2ms']);self.assertFalse(comparison['handoff_profile_equal'])
            self.assertTrue(comparison['prior_score_state_equal'])
            self.assertEqual(comparison['prior_keyon_age_difference_ms'],0)
            bad=copy.deepcopy(b);bad['old_note_count']-=1;bad['operand_reads']=[]
            self.assertFalse(reference.compare(a,bad)['prior_score_state_equal'])
            self.assertTrue(reference.compare(a,a)['handoff_profile_equal'])
            bad=copy.deepcopy(b);bad['wait_ms']+=2.01
            self.assertFalse(reference.compare(a,bad)['wait_within_2ms'])
            for value in (float('nan'),float('inf'),True):
                bad=copy.deepcopy(b);bad['wait_ms']=value
                with self.assertRaises(ValueError):reference.compare(a,bad)
            bad=copy.deepcopy(b);bad['startup_overlap']=True
            with self.assertRaises(ValueError):reference.compare(a,bad)
            changed=copy.deepcopy(a);changed['handoff']['music_writes'][0]['matching_reply_count']=1
            if live==32:self.assertFalse(reference.anchor(changed,live))
            changed=copy.deepcopy(a);changed['operand_before_directory']=False
            self.assertFalse(reference.anchor(changed,live))
            changed=copy.deepcopy(a);changed['handoff']['music_writes'].append(copy.deepcopy(changed['handoff']['music_writes'][-1]))
            self.assertFalse(reference.anchor(changed,live))

    def test_corpus_binding_failures_and_holdout_independence(self):
        rows=[row(c) for c in reference.cases()]
        gate=reference.corpus(rows)
        self.assertTrue(gate['passed']);self.assertFalse(gate['handoff_equivalent'])
        self.assertEqual(gate['original_retry_anchor_cases'],8)
        self.assertTrue(all(g['cases']==16 for g in gate['groups'].values()))
        for bad in (rows[:-1],rows+[rows[0]],rows[:-1]+[rows[0]]):
            with self.assertRaises(ValueError):reference.corpus(bad)
        for field,value in (('fixture_sha256','0'*64),('spin',True),('model','dmg')):
            bad=copy.deepcopy(rows);bad[0][field]=value
            with self.assertRaises(ValueError):reference.corpus(bad)
        bad=copy.deepcopy(rows);bad[-1]['replacement']['wait_ms']+=3
        # Caller-supplied success flags cannot bypass recomputed comparisons.
        bad[-1]['comparison']={'wait_within_2ms':True}
        self.assertFalse(reference.corpus(bad)['passed'])
        bad=copy.deepcopy(rows)
        for r in bad:
            if r['group']=='holdout':r['original']=copy.deepcopy(r['replacement'])
        gate=reference.corpus(bad)
        self.assertTrue(gate['passed']);self.assertTrue(gate['original_retry_anchors_passed'])
        bad=copy.deepcopy(rows)
        next(r for r in bad if r['model']=='sgb2')['original']['first_publication_before_operand']=False
        self.assertFalse(reference.corpus(bad)['passed'])

    def test_cli_reports_behavioral_failure_and_sanitizes_invalid_captures(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            group,m,live,gap,spin=game.stem.split('-');live,gap,spin=map(int,(live,gap,spin))
            self.assertEqual(m,model);self.assertEqual(clocks,300000000)
            self.assertEqual(flags,dict(tempo_publication=True))
            self.assertEqual(game.read_bytes(),reference.fixture((group,model,live,gap,spin)))
            return original_timeline(live) if program.name!='replacement.rom' else synthetic(live,8,position='before')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('probe','sgb1.program.rom','sgb2.program.rom'):(root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root)]
            output=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=capture) as run,contextlib.redirect_stdout(output):
                self.assertEqual(reference.main(),0)
            report=json.loads(output.getvalue());self.assertEqual(run.call_count,64)
            self.assertEqual(report['schema'],'gbb-sgb-vendor-publication-retry-v1')
            self.assertIs(report['qualification'],False);self.assertIs(report['parameters_fitted'],False)
            self.assertFalse(report['gate']['handoff_equivalent']);self.assertTrue(report['gate']['passed'])
            def mismatch(probe,program,game,model,clocks,inputs,**flags):
                if program.name=='replacement.rom':
                    return synthetic(int(game.stem.split('-')[2]),8,position='before',wait=35)
                return capture(probe,program,game,model,clocks,inputs,**flags)
            output=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=mismatch),contextlib.redirect_stdout(output):
                self.assertEqual(reference.main(),1)
            self.assertFalse(json.loads(output.getvalue())['gate']['passed'])
            for fault in (ValueError('private detail'),None):
                output=io.StringIO();error=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(output),contextlib.redirect_stderr(error):
                    with self.assertRaises(SystemExit) as raised:reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(output.getvalue(),'')
                self.assertEqual(error.getvalue(),'bounded publication-retry comparison failed; child diagnostics suppressed\n')

    def test_native_controls(self):
        if TIMELINE is None:self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                group,model,live,gap,spin=case;game=root/f'{group}-{model}-{live}-{gap}-{spin}.gb'
                game.write_bytes(reference.fixture(case))
                data=reference.capture(TIMELINE,program,game,model,300000000,inputs,tempo_publication=True)
                result=reference.observe(data,live)
                comparison=reference.compare(result,result)
                self.assertEqual(comparison['wait_error_ms'],0);self.assertTrue(comparison['handoff_profile_equal'])
                return result
            with ThreadPoolExecutor(max_workers=4) as pool:self.assertEqual(len(list(pool.map(run,reference.cases()))),32)

    def test_native_transparency_and_replay(self):
        if TIMELINE is None or PROBE is None:self.skipTest('both probes not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                group,model,live,gap,spin=case;game=root/f'{group}-{model}-{live}-{gap}-{spin}.gb'
                game.write_bytes(reference.fixture(case))
                captures=[reference.capture(TIMELINE,program,game,model,300000000,inputs,**flags)
                          for flags in (dict(tempo_boundary=True),dict(tempo_publication=True))]
                self.assertEqual(*captures);summarize(captures[1],live,8)
                results=[]
                for mode in ('native','scalar'):
                    child=subprocess.run([str(PROBE),str(program),str(game),model,'100000000',mode,'bundled'],capture_output=True,timeout=180)
                    self.assertEqual(child.returncode,0,child.stderr);self.assertLess(len(child.stdout),8192)
                    data=json.loads(child.stdout);self.assertTrue(data['reset_equal']);self.assertTrue(data['restore_equal'])
                    for key in ('selection_wait_restores','selection_wait_pulse_restores','unread_restores','nonzero'):self.assertGreater(data[key],0)
                    self.assertEqual((data['starts'],data['completes'],data['selected'],data['rejected']),(2,1,2,0))
                    results.append(data)
                for key in ('pcm_fnv64','frames','clocks','last_nonzero_clock','nonzero','notes'):self.assertEqual(results[0][key],results[1][key])
            with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(run,witnesses()))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path);parser.add_argument('--probe',type=Path)
    args,rest=parser.parse_known_args();TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
