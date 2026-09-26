"""Contract checks for SGB trace command inventory."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from report_sgb_commands import (count_commands, describe_sound, read_commands,
                                 report)  # noqa: E402


class SgbCommandReportTests(unittest.TestCase):
    def test_checked_in_fixture_reports_multiplayer(self) -> None:
        fixture = Path(__file__).parent / "fixtures/sgb/trace_fixture.trace"
        self.assertEqual(count_commands(fixture)[0x11], 1)
        self.assertIn("0x11 MLT_REQ", report([fixture]))

    def test_malformed_command_record_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.trace"
            path.write_text("GBB SGB trace\ncommand sequence=1 command=0x08 "
                            "bytes=16 packet=00\nend\n")
            with self.assertRaises(ValueError):
                count_commands(path)

    def test_duplicate_sequence_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.trace"
            command = "command sequence=1 command=0x08 bytes=16 packet=" + "41" + "00" * 15
            path.write_text("GBB SGB trace\n" + command + "\n" + command + "\nend\n")
            with self.assertRaises(ValueError):
                count_commands(path)

    def test_sound_requests_are_decoded_without_claiming_playback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sound.trace"
            packets = (
                (4, 0x08, bytes.fromhex("4103040c00" + "00" * 11)),
                (10, 0x09, bytes.fromhex("49" + "00" * 15)),
                (38, 0x08, bytes.fromhex("4100000002" + "00" * 11)),
            )
            lines = ["GBB SGB trace"]
            for sequence, command, packet in packets:
                lines.append(f"command sequence={sequence} command=0x{command:x} "
                             f"bytes=16 packet={packet.hex()}")
            path.write_text("\n".join((*lines, "end", "")), encoding="utf-8")
            records = read_commands(path)
            self.assertEqual(count_commands(path), {0x08: 2, 0x09: 1})
            self.assertIn("mute SNES sound", describe_sound(records[0]))
            self.assertIn("payload not in JOYP trace", describe_sound(records[1]))
            self.assertIn("score=0x02", describe_sound(records[2]))
            self.assertIn("#38: SOUND", report([path]))
            self.assertIn("not rendered by GBB", report([path]))


if __name__ == "__main__":
    unittest.main()
