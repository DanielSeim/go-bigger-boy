#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute owned vendor-format scores through GB LCD upload and real SPC/DSP."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_music_fixture import build, bank, payload, assets
from build_sgb_score_atomic_fixture import build_cartridge

PROBE = None
TIMELINE_PROBE = None


class VendorMusicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='gbb-vendor-music-')
        cls.root = Path(cls.directory.name)
        cls.rom = cls.root / 'host.rom'
        cls.rom.write_bytes(firmware())

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_image(self, image, model='sgb2', mode='native', clocks=30000000):
        game = self.root / 'game.gb'
        game.write_bytes(image)
        child = subprocess.run([str(PROBE), str(self.rom), str(game), model,
                                str(clocks), mode, 'fixture'], capture_output=True,
                               text=True, timeout=90)
        self.assertEqual(child.returncode, 0, child.stderr)
        result = json.loads(child.stdout)
        self.assertEqual(result['schema'], 'gbb-sgb-vendor-music-v1')
        self.assertIs(result['qualification'], False)
        self.assertIs(result['restore_equal'], True)
        self.assertIs(result['reset_equal'], True)
        self.assertGreater(result['restores'], 0)
        self.assertEqual(result['host_status'], 0)
        return result

    def test_01_build_and_export(self):
        image = firmware()
        self.assertEqual(image, firmware())
        self.assertEqual(hashlib.sha256(image).hexdigest(), '5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a')
        self.assertEqual(len(image), 262144)
        self.assertEqual(int.from_bytes(image[0x7FDC:0x7FDE], 'little') ^
                         int.from_bytes(image[0x7FDE:0x7FE0], 'little'), 65535)
        self.assertEqual(sum(image) & 65535, int.from_bytes(image[0x7FDE:0x7FE0], 'little'))
        for script, expected in (('build_sgb_vendor_music.py', image),
                                 ('build_sgb_vendor_music_fixture.py', build())):
            output = self.root / (script+'.bin')
            command = [sys.executable, str(Path(__file__).resolve().parents[1]/'scripts'/script), '--output', str(output)]
            first = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 2)
            self.assertEqual(output.read_bytes(), expected)

    def test_02_real_dsp_and_scalar(self):
        for model in ('sgb', 'sgb2'):
            native = self.run_image(build(), model)
            scalar = self.run_image(build(), model, 'scalar')
            self.assertEqual(native['pcm_fnv64'], scalar['pcm_fnv64'])
            self.assertEqual(native['frames'], scalar['frames'])
            for result in (native, scalar, self.run_image(build(), model, 'combined')):
                self.assertEqual((result['starts'], result['completes'], result['notes']), (1, 1, 3))
                self.assertEqual((result['transfers'], result['adoptions']), (1, 2))
                self.assertEqual(result['firmware_state'], 1)
                self.assertEqual(result['rejected'], 0)
                self.assertGreater(result['nonzero'], 1000)
                self.assertLess(result['last_nonzero_clock'], 20000000)
                self.assertEqual(result['flg'], 0)
            dry = self.run_image(build('dry'), model)
            self.assertNotEqual(native['pcm_fnv64'], dry['pcm_fnv64'])

    def test_03_separate_assets_and_lifecycle(self):
        for model in ('sgb', 'sgb2'):
            plain = self.run_image(build(), model)
            separate = self.run_image(build('split-assets'), model)
            self.assertEqual((separate['transfers'], separate['adoptions']), (2, 3))
            self.assertEqual((separate['starts'], separate['completes'], separate['notes']), (1, 1, 3))
            self.assertGreater(separate['nonzero'], 1000)
            for case, starts, completes, selected in (('switch', 3, 1, 3), ('stop', 1, 0, 1),
                                                      ('upload-active', 2, 1, 2)):
                result = self.run_image(build(case), model)
                self.assertEqual((result['starts'], result['completes'], result['selected']),
                                 (starts, completes, selected), (case, result))
                self.assertEqual(result['firmware_state'], 1)
                self.assertGreater(result['nonzero'], 100)
                self.assertEqual(result['flg'], 0xE0 if case == 'stop' else 0)
            muted = self.run_image(build('mute'), model)
            self.assertEqual((muted['notes'], muted['completes'], muted['nonzero']), (3, 1, 0))
            unmuted = self.run_image(build('unmute'), model)
            self.assertEqual(unmuted['completes'], 1)
            self.assertGreater(unmuted['nonzero'], 100)
            self.assertLess(unmuted['nonzero'], plain['nonzero'])

    def test_04_reject_before_any_note(self):
        for model in ('sgb', 'sgb2'):
            for fault in ('late-opcode', 'root-outside', 'root-before', 'channel',
                          'zero-tempo', 'instrument', 'filter', 'pan', 'truncated'):
                with self.subTest(model=model, fault=fault):
                    result = self.run_image(build(fault=fault), model)
                    self.assertEqual(result['firmware_state'], 255)
                    self.assertEqual(result['rejected'], 0xE2)
                    self.assertEqual((result['starts'], result['notes'], result['nonzero']), (0, 0, 0))
                    self.assertEqual(result['flg'], 0xE0)
            for case in ('code-write', 'asset-only-cold', 'bad-entry', 'asset-wrap', 'asset-overflow'):
                result = self.run_image(build(case), model)
                self.assertEqual(result['firmware_state'], 255)
                self.assertNotEqual(result['transfer_error'], 0)
                self.assertEqual((result['transfers'], result['starts'], result['nonzero']), (0, 0, 0))

    def test_06_owned_echo_profiles_and_full_send_mask(self):
        baseline = self.run_image(build())
        hashes = {baseline['pcm_fnv64']}
        for filter_id in (1, 2, 3):
            score = bytearray(bank())
            score[0x6D] = filter_id
            frame = payload(((0x2B00, bytes(score)),))
            image = build_cartridge((frame,), ((16,bytes((0x41,0,0,0,1)),None),))
            result = self.run_image(image)
            self.assertEqual((result['starts'], result['completes'], result['notes']), (1,1,3))
            self.assertGreater(result['nonzero'], 1000)
            hashes.add(result['pcm_fnv64'])
        self.assertEqual(len(hashes), 4)
        score = bytearray(bank())
        score[0x6F] = 255
        image = build_cartridge((payload(((0x2B00,bytes(score)),)),),
                                ((16,bytes((0x41,0,0,0,1)),None),))
        full_send = self.run_image(image)
        self.assertEqual(full_send['pcm_fnv64'], baseline['pcm_fnv64'])

    def test_07_private_report_validation_and_sanitization(self):
        from check_sgb_vendor_music_title import validate
        report = dict(schema='gbb-sgb-vendor-music-v1', qualification=False,
                      model='sgb2', mode='native', reset_equal=True, restore_equal=True,
                      firmware_state=1, transfer_error=0, transfers=2, adoptions=3,
                      sounds=3, starts=2, completes=1, notes=2, selected=1, rejected=0,
                      flg=0, host_status=0, external=0, uploaded_instrument=1, srcn=2,
                      pitch=1437, adsr1=255, adsr2=224, gain=184, clocks=1000000010,
                      frames=1489947, nonzero=60000, last_nonzero_clock=966000000,
                      restores=612, unread_restores=18, gb_frames=2668, pcm_fnv64=123)
        self.assertEqual(validate(report,'sgb2')['completes'],1)
        report['private_data'] = 'not forwarded'
        self.assertNotIn('private_data',validate(report,'sgb2'))
        for key in ('qualification','restore_equal','reset_equal','transfer_error','notes',
                    'completes','starts','rejected','uploaded_instrument','srcn','pitch','adsr1','gain','nonzero','unread_restores','clocks','frames'):
            changed = report.copy()
            changed[key] = True if key == 'qualification' else (1 if report[key] == 0 else 0)
            with self.subTest(key=key), self.assertRaises(ValueError): validate(changed,'sgb2')
        with self.assertRaises(ValueError): validate(report,'sgb')
        changed = report.copy(); changed['notes'] = True
        with self.assertRaises(ValueError): validate(changed,'sgb2')

    def test_09_reference_summary(self):
        from check_sgb_vendor_music_reference import title_summary
        events = [dict(half=120,register=0x4C,value=4,request=2,pitch=100,srcn=2),
                  dict(half=205,register=0x5C,value=255,request=3),
                  # Reselection's pre-key-on KOF is not the prior note's gate.
                  dict(half=208,register=0x5C,value=4,request=3),
                  dict(half=220,register=0x4C,value=4,request=3,pitch=100,srcn=2),
                  dict(half=300,register=0x5C,value=4,request=3),
                  dict(half=310,register=0x5C,value=255,request=3),
                  dict(half=312,register=0x6C,value=224,request=3)]
        report=dict(requests=[0,100,200],music=[0,1,1],events=events,private_data='discard')
        summary=title_summary(report)
        self.assertIs(summary['first_request_interrupted'],True)
        self.assertEqual(summary['last_gate_ms'],round(80/2048,6))
        self.assertNotIn('private_data',summary)
        for bad in (dict(report,music=[0,1,2]),dict(report,events=events[:4]),
                    dict(report,events=[events[0],dict(half=140,register=0x5C,value=4,request=2)]+events[1:])):
            with self.assertRaises(ValueError): title_summary(bad)

    def test_08_uploaded_instrument_binding(self):
        sound = bytes((0x41,0,0,0,1))
        def image(chunks, replacement=False):
            frames = [payload(((0x2B00,bank(echo=False)),)),payload(chunks)]
            events = [(4,bytes((0x49,)),1),(16,sound,None)]
            if replacement:
                frames.append(frames[0]); events = [(4,bytes((0x49,)),1),(4,bytes((0x49,)),2),(16,sound,None)]
            return build_cartridge(tuple(frames),tuple(events))
        for model in ('sgb','sgb2'):
            native = self.run_image(image(assets()),model)
            scalar = self.run_image(image(assets()),model,'scalar')
            self.assertEqual(native['pcm_fnv64'],scalar['pcm_fnv64'])
            self.assertEqual((native['uploaded_instrument'],native['srcn'],native['adsr1'],native['adsr2'],native['gain']), (1,2,0x8E,0xAF,0))
            self.assertEqual((native['notes'],native['completes']), (3,1))
            self.assertGreater(native['nonzero'],1000)
            half = self.run_image(image(assets(descriptor=(2,0x8E,0xAF,0,8,0))),model)
            self.assertEqual(half['pitch'],native['pitch']//2)
            self.assertNotEqual(half['pcm_fnv64'],native['pcm_fnv64'])
            oneshot = self.run_image(image(assets(sample=bytes((0xA1,))+bytes((0x22,))*8,loop=0)),model)
            self.assertEqual((oneshot['uploaded_instrument'],oneshot['completes']), (1,1))
            clipped = self.run_image(image(assets(descriptor=(2,0x8E,0xAF,0,255,255))),model)
            self.assertEqual(clipped['pitch'],16383)
            relocated = self.run_image(image(assets(start=0x3B04,loop=0x3B0D,
                sample=bytes(4)+bytes((0xA0,))+bytes((0x22,))*8+bytes((0xA3,))+bytes((0x33,))*8+bytes(3))),model)
            self.assertEqual((relocated['uploaded_instrument'],relocated['completes']), (1,1))
            chunked = list(assets(sample=bytes((0xA0,))+bytes((0x22,))*8+bytes((0xA3,))+bytes((0x33,))*8))
            chunked[-1:] = [(0x3B00,chunked[-1][1][:8]),(0x3B08,chunked[-1][1][8:])]
            fragmented = self.run_image(image(tuple(chunked)),model)
            self.assertEqual((fragmented['uploaded_instrument'],fragmented['completes']), (1,1))
            changed = self.run_image(image(assets(descriptor=(2,0x8F,0x6F,48,0x10,0))),model)
            self.assertEqual((changed['adsr1'],changed['adsr2'],changed['gain']), (0x8F,0x6F,48))
            resident = self.run_image(image(assets(),replacement=True),model)
            self.assertEqual((resident['uploaded_instrument'],resident['srcn']), (0,0))
            self.assertEqual(resident['completes'],1)
            faults = (assets(loop=0x3B01),assets(start=0x3AFF),assets(start=0x3B09),
                      assets(sample=bytes((0xA0,))+bytes(8)),
                      assets(sample=bytes((0xA2,))+bytes(8)),
                      assets(descriptor=(3,0x8E,0xAF,0,0x10,0)),
                      assets(descriptor=(2,0x8E,0xAF,0,0,0)))
            for chunks in faults:
                bad = self.run_image(image(chunks),model)
                self.assertEqual((bad['firmware_state'],bad['rejected'],bad['starts'],bad['nonzero']), (255,0xE2,0,0))
            for chunks in (assets()[:2],assets()[1:],assets()+assets()[:1]):
                bad = self.run_image(image(chunks),model)
                self.assertEqual((bad['firmware_state'],bad['starts'],bad['nonzero']), (255,0,0))
                self.assertNotEqual(bad['transfer_error'],0)

    def test_10_measured_timing_lifecycle(self):
        from build_sgb_vendor_timing_fixture import build as timing
        for model in ('sgb','sgb2'):
            for case in ('entry','entry-no-rest','short','entry-chain','pitch','stop','switch','upload'):
                with self.subTest(model=model,case=case):
                    native=self.run_image(timing(case),model,clocks=90000000)
                    self.assertEqual(native['rejected'],0)
                    self.assertEqual(native['firmware_state'],1)
                    self.assertGreater(native['nonzero'],100)
                    self.assertEqual(native['flg'],0xE0 if case=='stop' else 0)
                    self.assertEqual(native['starts'],2 if case in ('switch','upload') else 1)
                    self.assertEqual(native['completes'],0 if case=='stop' else 1)
                    self.assertEqual(native['uploaded_instrument'],1)
                    self.assertEqual(native['srcn'],2)
                    if case not in ('stop','switch','upload'):
                        self.assertEqual(native['notes'],14 if case=='pitch' else (3 if case=='entry-chain' else 1))
                    if case in ('entry','entry-chain'):
                        scalar=self.run_image(timing(case),model,'scalar',clocks=90000000)
                        self.assertEqual(native['pcm_fnv64'],scalar['pcm_fnv64'])
                    if case=='entry-no-rest': self.assertEqual(native['pitch'],1437)

    def test_11_public_timing_registers(self):
        from build_sgb_vendor_timing_fixture import build as timing
        from check_sgb_vendor_music_reference import timing_summary, check_timing
        if TIMELINE_PROBE is None: self.skipTest('timeline probe not supplied')
        inputs=self.root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
        for model in ('sgb','sgb2'):
            for case in ('entry','entry-no-rest','short','entry-chain','pitch'):
                game=self.root/'timing.gb'; game.write_bytes(timing(case))
                child=subprocess.run([str(TIMELINE_PROBE),str(self.rom),str(game),model,'90000000',str(inputs),'fixture'],capture_output=True,text=True,timeout=90)
                self.assertEqual(child.returncode,0,child.stderr)
                summary=timing_summary(json.loads(child.stdout))
                expected=[717,760,806,854,905,959,1015,1077,1142,1209,1281,1357,1437,1523]
                self.assertEqual(summary['pitches'],expected if case=='pitch' else ([1437,717,760] if case=='entry-chain' else [1437]))
                for value in summary['gates_ms']:
                    self.assertLess(abs(value-(72.4 if case in ('short','pitch') else 1066.4)),2)
                low,high={'entry':(1097,1105),'entry-no-rest':(1086,1095),'short':(86,95),'entry-chain':(1098,1108),'pitch':(80,90)}[case]
                self.assertLess(low,summary['completion_ms']); self.assertLess(summary['completion_ms'],high)
                for value in summary['gate_clear_ms']: self.assertLess(abs(value-0.611),0.02)
                self.assertLess(abs(summary['completion_clear_ms']-0.111),0.02)
                changed=dict(summary,pitches=[0])
                with self.assertRaises(ValueError): check_timing(summary,changed)
                changed=dict(summary,completion_ms=summary['completion_ms']+5)
                with self.assertRaises(ValueError): check_timing(summary,changed)

    def test_12_completed_retain_keeps_echo_silent(self):
        from build_sgb_vendor_timing_fixture import build as timing
        if TIMELINE_PROBE is None: self.skipTest('timeline probe not supplied')
        inputs=self.root/'retain.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
        for model in ('sgb','sgb2'):
            image=timing('retain')
            result=self.run_image(image,model,clocks=60000000)
            self.assertEqual((result['sounds'],result['starts'],result['completes']), (2,1,1))
            game=self.root/'retain.gb'; game.write_bytes(image)
            child=subprocess.run([str(TIMELINE_PROBE),str(self.rom),str(game),model,'60000000',str(inputs),'fixture'],capture_output=True,text=True,timeout=90)
            self.assertEqual(child.returncode,0,child.stderr)
            self.assertEqual(json.loads(child.stdout)['final'],dict(flg=0,kof=0,echo_left=0,echo_right=0))

    def test_05_native_execution_bounds(self):
        # A phrase-list loop made of nine valid references must reject, rather
        # than playing its first pattern. A control-only track reaches read cap.
        for kind in ('patterns', 'reads', 'events', 'end-word'):
            score = bytearray(bank())
            if kind == 'patterns':
                score[0x10:0x24] = (0x2B40).to_bytes(2, 'little')*9 + bytes(2)
                score[0x40:0x50] = bytes(4)+(0x2B60).to_bytes(2, 'little')+bytes(10)
            elif kind == 'reads':
                score = bytearray(bank())+bytearray(3800)
                score[0x60:] = bytes((0xE1,10))*((len(score)-0x60)//2)
            elif kind == 'events':
                score = bytearray(bank())+bytearray(256)
                score[0x60:0x60+134] = bytes((1,127))+bytes((0x98,))*131+bytes(1)
            else:
                score[0:2] = (0x2BFF).to_bytes(2, 'little')
            frame = payload(((0x2B00, bytes(score)),))
            image = build_cartridge((frame,), ((16,bytes((0x41,0,0,0,1)),None),))
            result = self.run_image(image)
            self.assertEqual((result['firmware_state'], result['rejected'], result['nonzero']), (255, 0xE2, 0), (kind, result))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--timeline-probe',type=Path)
    args, rest = parser.parse_known_args()
    PROBE = args.probe.resolve()
    TIMELINE_PROBE = args.timeline_probe.resolve() if args.timeline_probe else None
    unittest.main(argv=[sys.argv[0]]+rest)
