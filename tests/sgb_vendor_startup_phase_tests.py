#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent startup anchors, age-only calibration and fail-closed phase checks."""
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
import check_sgb_vendor_startup_phase_reference as reference
from sgb_vendor_publication_retry_tests import original_timeline
from sgb_vendor_delayed_tempo_tests import synthetic
from build_sgb_vendor_music import build as firmware

TIMELINE=PROBE=None
REDUCTIONS={'sgb':7,'sgb2':3}


def result(live=32):return reference.observe(original_timeline(live),live)


def calibration():return {m:dict(request_gap_reduction=r) for m,r in REDUCTIONS.items()}


def rows():
    return [dict(group=g,model=m,live=l,gap=gap,spin=s,
        original_fixture_sha256=hashlib.sha256(reference.fixture((g,m,l,gap,s))).hexdigest(),
        replacement_fixture_sha256=hashlib.sha256(reference.adjusted_fixture((g,m,l,gap,s),REDUCTIONS[m])).hexdigest(),
        original=result(l),replacement=result(l)) for g,m,l,gap,s in reference.cases()]


class StartupPhaseContracts(unittest.TestCase):
    def test_startup_decomposition_and_ambiguity(self):
        data=original_timeline(32);r=reference.observe(data,32)
        startup=r['startup']
        self.assertAlmostEqual(sum(startup[k] for k in ('request_to_directory_ms','directory_to_root_ms','root_to_keyon_ms')),startup['request_to_keyon_ms'])
        self.assertAlmostEqual(r['request_spacing_ms']-startup['request_to_keyon_ms'],r['prior_keyon_age_ms'])
        self.assertEqual(r['notes_before_request'],[1437,1357,1437])
        self.assertIs(r['tempo_read_before_request'],False)
        self.assertFalse(r['pre_request_boundary_ambiguous'])
        for fault in ('directory','root','order'):
            bad=copy.deepcopy(data)
            if fault=='directory':bad['selection_directory_reads']=[x for x in bad['selection_directory_reads'] if x['address']!=0x2B00]
            elif fault=='root':bad['selection_traversal_reads']=[x for x in bad['selection_traversal_reads'] if x['address']!=0x2B10]
            else:bad['requests'][0]=r['first_old_keyon_half']
            with self.assertRaises((ValueError,StopIteration)):reference.observe(bad,32)
        bad=copy.deepcopy(data);bad['tempo_operand_reads']*=2
        r=reference.observe(bad,32)
        self.assertIsNone(r['tempo_read_before_request']);self.assertTrue(r['pre_request_boundary_ambiguous'])
        self.assertFalse(reference.compare(r,r)['phase_aligned'])
        tied=copy.deepcopy(data)
        third=[e for e in tied['events'] if e['register']==0x4C][2]
        third['half']=tied['requests'][1];third['request']=2
        r=reference.observe(tied,32)
        self.assertTrue(r['pre_request_boundary_ambiguous'])
        self.assertFalse(reference.compare(r,r)['phase_aligned'])

    def test_age_calibration_ignores_waits_and_bounds_fixtures(self):
        a,b,c=[dict(prior_keyon_age_ms=n) for n in (70,180,164)]
        fitted=reference.calibrate(a,b,c);self.assertEqual(fitted['request_gap_reduction'],7)
        b['wait_ms']=float('nan');b['handoff']='irrelevant'
        self.assertEqual(reference.calibrate(a,b,c),fitted)
        for ages in ((70,180,180),(70,180,179),(70,400,384),(float('nan'),180,164),(70,True,164)):
            with self.assertRaises(ValueError):reference.calibrate(*(dict(prior_keyon_age_ms=n) for n in ages))
        for case in reference.cases():
            image=reference.adjusted_fixture(case,REDUCTIONS[case[1]])
            self.assertEqual(image[0x4000:],reference.fixture(case)[0x4000:])
        for reduction in (True,-1,10):
            with self.assertRaises(ValueError):reference.adjusted_fixture(reference.cases()[0],reduction)
        with self.assertRaises(ValueError):reference.adjusted_fixture(reference.cases()[0],9)

    def test_phase_checks_and_corpus_identity(self):
        a=result();b=copy.deepcopy(a)
        self.assertTrue(reference.compare(a,b)['phase_controlled_wait_agreement'])
        for field,value in (('prior_keyon_age_ms',a['prior_keyon_age_ms']+2.01),
                            ('notes_before_request',[1437]),('tempo_read_before_request',True),
                            ('pre_request_boundary_ambiguous',True)):
            bad=copy.deepcopy(b);bad[field]=value
            compared=reference.compare(a,bad)
            self.assertFalse(compared['phase_aligned']);self.assertIsNone(compared['phase_controlled_wait_agreement'])
        bad=copy.deepcopy(b);bad['wait_ms']+=3
        self.assertTrue(reference.compare(a,bad)['phase_aligned'])
        self.assertFalse(reference.compare(a,bad)['phase_controlled_wait_agreement'])
        corpus=rows();self.assertTrue(reference.corpus(corpus,calibration())['passed'])
        for bad in (corpus[:-1],corpus+[corpus[0]],corpus[:-1]+[corpus[0]]):
            with self.assertRaises(ValueError):reference.corpus(bad,calibration())
        bad=copy.deepcopy(corpus);bad[0]['replacement_fixture_sha256']='0'*64
        with self.assertRaises(ValueError):reference.corpus(bad,calibration())
        bad=copy.deepcopy(corpus);bad[0]['replacement']['prior_keyon_age_ms']+=5
        bad[0]['comparison']={'phase_aligned':True}
        self.assertFalse(reference.corpus(bad,calibration())['passed'])

    def test_cli_scope_fitting_disclosure_and_failures(self):
        def capture(probe,program,game,model,clocks,inputs,**flags):
            self.assertEqual(clocks,300000000);self.assertEqual(flags,dict(tempo_publication=True))
            if '-cal-' in game.stem:
                data=original_timeline(128)
                # Equal programs give a zero reduction; move only the synthetic
                # request witness for the earlier unchanged-tempo fixture.
                if game.stem.endswith('-earlier'):
                    data=copy.deepcopy(data);data['requests'][1]-=32768
                    data['timer_continuity'][0]['end_half']=data['requests'][1]
                    for access in data['timer_continuity'][0]['playback']:
                        for key in ('all','nonzero'):
                            stats=access[key]
                            if stats['count']:
                                stats['last']=data['requests'][1]-100
                                stats['min_gap']=stats['max_gap']=stats['last']-stats['first']
                return data
            return original_timeline(int(game.stem.split('-')[2]))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('probe','sgb1.program.rom','sgb2.program.rom'):(root/name).write_bytes(b'owned stub')
            argv=['check','--probe',str(root/'probe'),'--firmware-dir',str(root)]
            output=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=capture) as child,contextlib.redirect_stdout(output):
                self.assertEqual(reference.main(),0)
            report=json.loads(output.getvalue());self.assertEqual(child.call_count,70)
            self.assertIs(report['qualification'],False);self.assertIs(report['parameters_fitted'],True)
            self.assertEqual(report['fitted_parameters'],['replacement_request_gap_frames'])
            for m in REDUCTIONS:self.assertEqual(report['calibration'][m]['request_gap_reduction'],0)
            def mismatch(*args,**kwargs):
                data=capture(*args,**kwargs)
                if args[1].name=='replacement.rom' and '-cal-' not in args[2].stem:
                    data=synthetic(int(args[2].stem.split('-')[2]),8,position='between',wait=35)
                return data
            output=io.StringIO()
            with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=mismatch),contextlib.redirect_stdout(output):
                self.assertEqual(reference.main(),1)
            self.assertFalse(json.loads(output.getvalue())['gate']['passed'])
            for fault in (ValueError('private detail'),None):
                output=io.StringIO();error=io.StringIO()
                with patch.object(sys,'argv',argv),patch.object(reference,'capture',side_effect=fault,return_value={}),contextlib.redirect_stdout(output),contextlib.redirect_stderr(error):
                    with self.assertRaises(SystemExit) as raised:reference.main()
                self.assertEqual(raised.exception.code,2);self.assertEqual(output.getvalue(),'')
                self.assertEqual(error.getvalue(),'bounded startup-phase comparison failed; child diagnostics suppressed\n')

    def test_native_adjusted_controls(self):
        if TIMELINE is None:self.skipTest('timeline probe not supplied')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                g,m,l,gap,s=case;game=root/f'{g}-{m}-{l}-{gap}-{s}.gb'
                game.write_bytes(reference.adjusted_fixture(case,REDUCTIONS[m]))
                data=reference.capture(TIMELINE,program,game,m,300000000,inputs,tempo_publication=True)
                r=reference.observe(data,l);self.assertTrue(reference.compare(r,r)['phase_aligned'])
                return r
            with ThreadPoolExecutor(max_workers=4) as pool:self.assertEqual(len(list(pool.map(run,reference.cases()))),32)

    def test_native_transparency_and_replay(self):
        if TIMELINE is None or PROBE is None:self.skipTest('both probes not supplied')
        from sgb_vendor_publication_retry_tests import witnesses
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);program=root/'owned.rom';program.write_bytes(firmware(selection_wait=True))
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                g,m,l,gap,s=case;game=root/f'{g}-{m}-{l}-{gap}-{s}.gb';game.write_bytes(reference.adjusted_fixture(case,REDUCTIONS[m]))
                data=[reference.capture(TIMELINE,program,game,m,300000000,inputs,**flag) for flag in
                      (dict(tempo_boundary=True),dict(tempo_publication=True))]
                self.assertEqual(*data);reference.observe(data[1],l)
                results=[]
                for mode in ('native','scalar'):
                    child=subprocess.run([str(PROBE),str(program),str(game),m,'100000000',mode,'bundled'],capture_output=True,timeout=180)
                    self.assertEqual(child.returncode,0,child.stderr);self.assertLess(len(child.stdout),8192)
                    r=json.loads(child.stdout);self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
                    for k in ('selection_wait_restores','selection_wait_pulse_restores','unread_restores','nonzero'):self.assertGreater(r[k],0)
                    self.assertEqual((r['starts'],r['completes'],r['selected'],r['rejected']),(2,1,2,0));results.append(r)
                for k in ('pcm_fnv64','frames','clocks','last_nonzero_clock','nonzero','notes'):self.assertEqual(results[0][k],results[1][k])
            with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(run,witnesses()))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--timeline-probe',type=Path);parser.add_argument('--probe',type=Path)
    args,rest=parser.parse_known_args();TIMELINE=args.timeline_probe.resolve() if args.timeline_probe else None
    PROBE=args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
