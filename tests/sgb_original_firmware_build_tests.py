#!/usr/bin/env python3
"""Validate original prototype image layout and assembler rejection boundaries."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('prototype', ROOT / 'scripts/build_sgb_prototype.py')
prototype = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prototype)


class BuildContracts(unittest.TestCase):
    def test_lorom_layout_and_checksum(self):
        rom, host, payload = prototype.build()
        self.assertEqual(len(rom), 0x40000)
        self.assertEqual(rom[:len(host)], host)
        self.assertEqual(rom[0x1000:0x1000+len(payload)], payload)
        self.assertEqual(rom[0x7fd5], 0x20)
        self.assertEqual(rom[0x7ffc:0x7ffe], b'\x00\x80')
        complement = int.from_bytes(rom[0x7fdc:0x7fde], 'little')
        checksum = int.from_bytes(rom[0x7fde:0x7fe0], 'little')
        self.assertEqual(complement ^ checksum, 65535)
        self.assertEqual(sum(rom) & 65535, checksum)
        self.assertEqual(rom, prototype.build()[0])
        self.assertGreater(len(payload), 256, 'exercise real upload counter wraps')
        # Directory and authored BRR blocks remain disjoint from program code.
        self.assertEqual(payload[0x300:0x310], bytes.fromhex('0006000609060906120612061b061b06'))
        self.assertEqual(payload[0x400] & 3, 3, 'square BRR end+loop')
        self.assertEqual(payload[0x409] & 3, 3, 'triangle BRR end+loop')
        self.assertEqual(payload[0x412] & 3, 3, 'pulse BRR end+loop')
        self.assertEqual(payload[0x41b] & 3, 3, 'saw BRR end+loop')
        self.assertEqual(payload[0x440:0x442], bytes.fromhex('e414'), 'fixed effect module precedes music')
        self.assertEqual(payload[0x5d0:0x5e0], bytes.fromhex('04050608060504030806040609060403'))
        driver = prototype.assemble((prototype.FIRMWARE/'driver.asm').read_text(), 'spc', prototype.DRIVER_ADDRESS)
        self.assertEqual(payload[:6], bytes.fromhex('8f00305f0010'), 'legacy entry selects v4 and jumps to relocated resident')
        self.assertEqual(payload[prototype.DRIVER_OFFSET:prototype.DRIVER_OFFSET+len(driver)], driver)
        self.assertLessEqual(len(driver), 766,
                             'driver must fit transfer fixtures without fixed assets')
        entries = prototype.assemble((prototype.FIRMWARE/'entries.asm').read_text(), 'spc', 0x200)
        self.assertEqual(payload[0x200:len(entries)], entries[0x200:])
        self.assertEqual(payload[0x600:prototype.DRIVER_OFFSET], bytes(prototype.DRIVER_OFFSET-0x600),
                         'future track workspace must be disjoint from resident code')

    def test_score_interpreter_encodings(self):
        source = 'call $0700\nmov a, $07d0+x\nadc a, $20\nmov a, #$60\nret'
        self.assertEqual(prototype.assemble(source, 'spc', 0x200), bytes.fromhex('3f0007f5d0078420e8606f'))
        self.assertEqual(prototype.assemble('mov a, x\nmov x, $32\nmov a, x\ncmp a, $31\ninc $32\nbcs $020a', 'spc', 0x200),
                         bytes.fromhex('7df8327d6431ab32b000'))

    def test_reject_unsafe_encodings(self):
        cases = [
            ('same:\nsame:', 'host', 0x8000),
            ('lda #$100', 'host', 0x8000),
            ('ldx #$10000', 'host', 0x8000),
            ('mov $100, a', 'spc', 0x200),
            ('mov a, $10000+x', 'spc', 0x200),
            ('call $10000', 'spc', 0x200),
            ('adc a, $100', 'spc', 0x200),
            ('.org $01ff', 'spc', 0x200),
            ('bra far\n.org $8100\nfar:', 'host', 0x8000),
            ('bra $7f81', 'host', 0x8000),
            ('cop #$00', 'host', 0x8000),
        ]
        for source, cpu, origin in cases:
            with self.subTest(source=source):
                with self.assertRaises(ValueError):
                    prototype.assemble(source, cpu, origin)
        # Both signed branch endpoints are representable.
        self.assertEqual(prototype.assemble('bra $7f82', 'host', 0x8000), b'\x80\x80')
        self.assertEqual(prototype.assemble('bra $8081', 'host', 0x8000), b'\x80\x7f')


if __name__ == '__main__':
    unittest.main()
