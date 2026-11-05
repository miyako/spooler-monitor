"""Next-version computation and version-file rewriting used by the Publish workflow."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import version  # noqa: E402


class VersionTests(unittest.TestCase):
    def test_bumps_latest_release_tag_including_legacy_prefix(self):
        tags = ["v0.1.0", "0.0.9", "0.2.0-rc.1", "1.0.0+build", "feature", "v0.1"]
        expected = {"patch": "0.1.1", "minor": "0.2.0", "major": "1.0.0"}
        for mode, result in expected.items():
            with self.subTest(mode=mode):
                self.assertEqual(version.next_version(ROOT, mode, tags), result)
        self.assertEqual(version.next_version(ROOT, "patch", ["0.1.1", "v0.1.0", "0.1.10"]), "0.1.11")

    def test_no_tags_starts_from_configured_version(self):
        current = json.loads((ROOT / "template.json").read_text())["version"]
        major, minor, patch = map(int, current.split("."))
        self.assertEqual(version.next_version(ROOT, "patch", []), f"{major}.{minor}.{patch + 1}")

    def test_rejects_unknown_mode(self):
        with self.assertRaises(ValueError):
            version.bump((0, 1, 0), "build")

    def test_every_version_file_carries_the_configured_version(self):
        current = json.loads((ROOT / "template.json").read_text())["version"]
        self.assertEqual(version.set_version(ROOT, current), [])

    def test_set_version_rewrites_all_version_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = json.loads((ROOT / "template.json").read_text())
            for name in version.VERSION_FILES:
                name = name.format(repository=config["repository"])
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, root / name)
            changed = version.set_version(root, "9.8.7")
            self.assertEqual(len(changed), len(version.VERSION_FILES))
            for path in changed:
                content = path.read_text(encoding="utf-8")
                self.assertIn("9.8.7", content)
                self.assertNotIn('"' + config["version"] + '"', content.replace('\\"', '"'))
            self.assertEqual(json.loads((root / "package.json").read_text())["tool"]["version"], "20.7")
            with self.assertRaises(ValueError):
                version.set_version(root, "v9.8.8")


if __name__ == "__main__":
    unittest.main()
