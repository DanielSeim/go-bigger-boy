#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject unexecuted subframe delays, inconsistent GB clocks and phase replay."""
import copy
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_bank_cold_phase_audio_fixture import build,SPINS,ORDERS,PROFILES
from build_sgb_bank_cold_mixed_audio_fixture import build as mixed_build
from check_sgb_bank_cold_phase_audio import observe,compare,MASTER
from sgb_bank_cold_mixed_tail_audio_tests import synthetic as tail_synthetic


def synthetic(order='gap-root',profile='both',voice=2,model='sgb'):
    m,raw=tail_synthetic(order,profile,voice,model)
    m.update(command_phase='staggered',phase_model=model,command_phases=[])
    for i,e in enumerate(m['edges']):
        elapsed=e[3]-m['edges'][0][3]
        cycles=elapsed//5 if model=='sgb' else elapsed*4194304//MASTER
        offset=(28*SPINS[i]+8 if SPINS[i] else 0)+48
        line,dot=divmod((144*456+offset)%70224,456)
        m['command_phases'].append([i+1,1000000+cycles,line,dot])
    return m,raw


def controls(order='gap-root',voice=2,model='sgb'):
    return {p:observe(*synthetic(order,p,voice,model),order,p,voice) for p in PROFILES}


class ColdPhaseAudioTests(unittest.TestCase):
    def test_delay_instructions_payloads_and_exports(self):
        for order in ORDERS:
            for voice in (2,3):
                for profile in PROFILES:
                    image=build(order,profile,voice);old=mixed_build(order,profile,voice,semantic_tail=True)
                    self.assertEqual(image,build(order,profile,voice));self.assertEqual(len(image),32768)
                    self.assertEqual(image[0x4000:],old[0x4000:])
                    code=image[0x150:0x4000]
                    for n in SPINS:
                        if n:self.assertEqual(code.count(bytes((1,n&255,n>>8,0x0B,0x78,0xB1,0x20,0xFB))),1)
                    both=build(order,'both',voice)
                    changes=[(a,b) for a,b in zip(both[0x150:0x4000],code) if a!=b]
                    self.assertEqual(changes,[(16,0)] if profile=='native' else [])
                    self.assertEqual(image[0x14D],(-sum(image[0x134:0x14D])-25)&255)
                    self.assertEqual(struct.unpack_from('>H',image,0x14E)[0],(sum(image)-sum(image[0x14E:0x150]))&65535)
        for delays in ([],[0]*7,[0]*9,[True]*8,[-1]*8,[2500]*8,'bad'):
            with self.assertRaises(ValueError):mixed_build(semantic_tail=True,spin_delays=delays)
        for args in (('bad',),('gap-root','bad'),('gap-root','both',True)):
            with self.assertRaises(ValueError):build(*args)
        with tempfile.TemporaryDirectory() as name:
            output=Path(name)/'cold-phase.gb'
            command=[sys.executable,str(Path(__file__).resolve().parents[1]/'scripts/build_sgb_bank_cold_phase_audio_fixture.py'),
                     '--order','root-gap','--voice','3','--profile','native','--output',str(output)]
            for code in (0,2):
                result=subprocess.run(command,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,code,result.stderr);self.assertEqual(output.read_bytes(),build('root-gap','native',3))

    def test_both_models_voices_orders_and_source_phases(self):
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for order in ORDERS:
                    result=compare(controls(order,voice,model),model,voice)
                    self.assertEqual(result['maximum_sum_error_lsb'],dict(left=0,right=0))
                    self.assertEqual(result['spin_counts'],list(SPINS))
                    self.assertEqual(len(result['gb_pulse_windows']),7)

    def test_wrong_or_unexecuted_phase_and_clock_rejected(self):
        for model in ('sgb','sgb2'):
            original,raw=synthetic(model=model)
            for mutation in ('identity','model','missing','extra','stage','bool','clock','backward','line','dot'):
                m=copy.deepcopy(original)
                if mutation=='identity':m['command_phase']='baseline'
                elif mutation=='model':m['phase_model']='dmg'
                elif mutation=='missing':m['command_phases'].pop()
                elif mutation=='extra':m['command_phases'].append(m['command_phases'][-1].copy())
                elif mutation=='stage':m['command_phases'][3][0]=3
                elif mutation=='bool':m['command_phases'][0][1]=True
                elif mutation=='clock':m['command_phases'][4][1]+=1000
                elif mutation=='backward':m['command_phases'][1][1]=1
                elif mutation=='line':m['command_phases'][4][2]=154
                else:m['command_phases'][4][3]=456
                with self.subTest(model=model,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,'gap-root','both',2)
            for i,n in enumerate(SPINS):
                if not n:continue
                for offset in (28*n+7,28*n+8+129):
                    m=copy.deepcopy(original);m['command_phases'][i][2:]=divmod((144*456+offset)%70224,456)
                    with self.subTest(model=model,stage=i+1,offset=offset),self.assertRaises(ValueError):observe(m,raw,'gap-root','both',2)
            for i in (2,4,6,7):
                m=copy.deepcopy(original);m['command_phases'][i][2:]=[144,48]
                with self.subTest(model=model,unexecuted=i+1),self.assertRaises(ValueError):observe(m,raw,'gap-root','both',2)

    def test_phase_cannot_hide_stale_token_or_source_timeline(self):
        for order in ORDERS:
            original,raw=synthetic(order)
            for mutation in ('token','error','handoff','blocked','admission','queued','note'):
                m=copy.deepcopy(original)
                if mutation=='token':m['consumed_tokens'][-1]=0
                elif mutation=='error':m['rejects'][3][5]=m['rejects'][0][5]
                elif mutation=='handoff':m['rejects'][3][6]=2
                elif mutation=='blocked':m['rejects'][4][4]=0
                elif mutation=='admission':m['recovered'][7]=1
                elif mutation=='queued':m['queued_rejects']=15
                else:m['notes'][0][6]=98000
                with self.subTest(order=order,mutation=mutation),self.assertRaises(ValueError):observe(m,raw,order,'both',2)
        for mutation in ('source_phase','source_model'):
            c=controls()
            if mutation=='source_phase':c['native']['meta']['command_phases'][4][3]+=1
            else:c['gb']['meta']['phase_model']='sgb2'
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):compare(c,'sgb',2)


if __name__=='__main__':unittest.main()
