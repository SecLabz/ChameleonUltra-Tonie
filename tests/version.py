"""Check release identity generation and reject unsafe version strings."""

import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "build_version",
    pathlib.Path(__file__).resolve().parents[1] / "tools/build_version.py",
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class VersionTests(unittest.TestCase):
    def test_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory) / "build/firmware_version.h"
            for version in ("0.1.0-dev", "v1.2.3", "v1.2.3-rc.1+build.7"):
                module.write_version(version, destination)
                self.assertIn(f'"{version}"', destination.read_text())

    def test_unsafe_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory) / "firmware_version.h"
            for version in ("", "v1/2", 'v1"', "v1\n", "v1\\2", "v" * 65):
                with self.subTest(version=version), self.assertRaises(ValueError):
                    module.write_version(version, destination)
                self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
