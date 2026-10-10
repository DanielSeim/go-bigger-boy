import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "prepare_play_release_notes",
    Path(__file__).resolve().parents[1] / "scripts/prepare_play_release_notes.py",
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PlayReleaseNotesTests(unittest.TestCase):
    url = "https://github.com/example/gbb/releases/tag/v1.2.3"

    def notes(self, body):
        return module.play_release_notes(body, self.url)

    def test_description_becomes_plain_text(self):
        self.assertEqual(
            self.notes("## Changes\r\n\r\n- Fix **audio** &amp; `video`.\r\n"
                       "- [Details](https://example.com).<!-- hidden -->"),
            "Changes\n\n- Fix audio & video.\n- Details.",
        )

    def test_short_description_preserved(self):
        self.assertEqual(self.notes("Fix sound.\nImprove controls."),
                         "Fix sound.\nImprove controls.")

    def test_exact_limit_preserved(self):
        self.assertEqual(self.notes("x" * 500), "x" * 500)

    def test_long_description_has_excerpt_and_release_link(self):
        notes = self.notes("x" * 501)
        self.assertEqual(len(notes), 500)
        self.assertTrue(notes.startswith("xxx"))
        self.assertTrue(notes.endswith(f"…\n\nFull release notes: {self.url}"))

    def test_unicode_fits_limit_without_splitting_characters(self):
        notes = self.notes("🎮" * 500)
        self.assertLessEqual(len(notes.encode("utf-16-le")) // 2, 500)
        self.assertTrue(notes.startswith("🎮"))
        self.assertTrue(notes.endswith(self.url))

    def test_empty_description_rejected(self):
        for body in ("", " \n ", "<!-- hidden -->"):
            with self.subTest(body=body), self.assertRaises(ValueError):
                self.notes(body)

    def test_cli_writes_utf8_metadata_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "release.json"
            output = root / "release-notes/en-US/default.txt"
            source.write_text(json.dumps({
                "body": "## Update\n\n🎮 Fix audio.", "html_url": self.url,
            }), encoding="utf-8")
            subprocess.run([sys.executable, str(spec.origin), str(source),
                            str(output)], check=True, capture_output=True)
            self.assertEqual(output.read_text(encoding="utf-8"),
                             "Update\n\n🎮 Fix audio.")


if __name__ == "__main__":
    unittest.main()
