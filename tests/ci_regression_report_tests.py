#!/usr/bin/env python3
"""Unit tests for the dependency-free CI regression report generator."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import ci_regression_report


class RegressionReportTests(unittest.TestCase):
    def test_combines_ctest_timing_and_link_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            junit = root / "ctest.xml"
            junit.write_text(
                """<testsuites><testsuite name='accuracy'>
                <testcase classname='mooneye' name='timer.gb' time='1.25'/>
                <testcase classname='link' name='tcp' time='0.5'>
                  <failure message='transfer failed'>details</failure>
                </testcase>
                <testcase name='skipped'><skipped/></testcase>
                </testsuite></testsuites>""",
                encoding="utf-8",
            )
            link_root = root / "link"
            link_root.mkdir()
            (link_root / "disconnect.report").write_text(
                "fault_plan=disconnect\n"
                "fault_events_applied=1\n"
                "host_transfers_completed=0\n"
                "join_transfers_completed=0\n",
                encoding="utf-8",
            )
            (link_root / "failure.txt").write_text(
                "handshake did not become ready\n", encoding="utf-8"
            )

            ctest = ci_regression_report.parse_junit([junit])
            link = ci_regression_report.parse_link_diagnostics(link_root)
            report = {
                "status": ci_regression_report.overall_status(ctest, link),
                "ctest": ctest,
                "link_diagnostics": link,
            }

            self.assertEqual(ctest["counts"], {
                "passed": 1,
                "failed": 1,
                "skipped": 1,
                "error": 0,
            })
            self.assertEqual(ctest["duration_seconds"], 1.75)
            self.assertEqual(report["status"], "FAIL")
            markdown = ci_regression_report.markdown_report(report)
            self.assertIn("Passed: **1**, failed: **1**", markdown)
            self.assertIn("mooneye::timer.gb", markdown)
            self.assertIn("link::tcp", markdown)
            self.assertIn("handshake did not become ready", markdown)
            self.assertIn("disconnect.report", markdown)

            output = root / "report.json"
            output.write_text(json.dumps(report), encoding="utf-8")
            self.assertEqual(json.loads(output.read_text())["status"], "FAIL")

            missing = ci_regression_report.parse_junit([root / "missing.xml"])
            self.assertEqual(
                ci_regression_report.overall_status(
                    missing, {"reports": [], "failure_files": []}
                ),
                "INCOMPLETE",
            )


if __name__ == "__main__":
    unittest.main()
