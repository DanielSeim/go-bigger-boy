import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "extract_changelog_release",
    Path(__file__).resolve().parents[1] / "scripts/extract_changelog_release.py",
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ChangelogReleaseNotesTests(unittest.TestCase):
    def test_only_matching_entry_with_markdown_preserved(self):
        changelog = ("# Changelog\n\n## [Unreleased]\n\n- Future\n\n"
                     "## [1.2.3] - 2026-10-11\n\n### Fixes\n\n"
                     "- **Audio** & [details](https://example.com)\n  continuation\n\n"
                     "## [1.2.2] - 2026-10-10\n\n- Old\n")
        self.assertEqual(module.release_notes(changelog, "v1.2.3"),
                         "### Fixes\n\n- **Audio** & [details](https://example.com)\n"
                         "  continuation\n")

    def test_last_entry_without_final_newline(self):
        self.assertEqual(module.release_notes("## [1.2.3]\n\n- Fix 🎮", "v1.2.3"),
                         "- Fix 🎮\n")

    def test_version_prefix_is_not_a_match(self):
        with self.assertRaises(ValueError):
            module.release_notes("## [1.2.30]\n\n- Wrong version\n", "v1.2.3")

    def test_duplicate_entries_rejected(self):
        with self.assertRaises(ValueError):
            module.release_notes("## [1.2.3]\n- One\n## [1.2.3]\n- Two\n", "v1.2.3")

    def test_empty_entry_rejected(self):
        with self.assertRaises(ValueError):
            module.release_notes("## [1.2.3]\n\n## [1.2.2]\n- Old\n", "v1.2.3")

    def test_fenced_headings_do_not_split_or_duplicate_entries(self):
        for fence in ("```", "~~~~"):
            body = f"Example:\n\n{fence}markdown\n## [1.2.3]\n## Example\n{fence}\n"
            self.assertEqual(module.release_notes("## [1.2.3]\n\n" + body +
                                                 "\n## [1.2.2]\n- Old\n", "v1.2.3"), body)

    def test_invalid_tag_rejected(self):
        for tag in ("main", "1.2.3", "v1.2.3-beta", "v1.2.3\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                module.release_notes("## [1.2.3]\n- Fix\n", tag)

    def test_cli_extracts_utf8_and_fails_for_missing_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "CHANGELOG.md", root / "notes.md"
            source.write_text("## [1.2.3]\n\n- Fix 🎮\n", encoding="utf-8")
            command = [sys.executable, str(spec.origin), str(source)]
            subprocess.run(command + ["v1.2.3", str(output)], check=True,
                           capture_output=True)
            self.assertEqual(output.read_text(encoding="utf-8"), "- Fix 🎮\n")
            result = subprocess.run(command + ["v9.9.9", str(output)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
