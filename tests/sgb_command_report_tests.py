"""Contract checks for SGB trace command inventory."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from report_sgb_commands import count_commands, report  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
