"""Real DFU checks with synthetic firmware and published/disposable keys."""

import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import release


def record(address, kind, data):
    raw = bytes((len(data), address >> 8, address & 255, kind)) + data
    return ":" + (raw + bytes((-sum(raw) & 255,))).hex().upper() + "\n"


def rewrite_package(source, destination, transform):
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(destination, "w") as dst:
        for name in src.namelist():
            dst.writestr(name, transform(name, src.read(name)))


class ReleaseTests(unittest.TestCase):
    def test_versions(self):
        for tag in ("v0.1.0", "v1.0.0", "v2.2.0", "v1.2.3-rc.1", "v1.2.3+build.7"):
            self.assertEqual(release.application_version(tag), 1)
        self.assertEqual(release.application_version("v-custom", "42"), 42)
        for tag, override in (
            ("v-custom", ""),
            ("v1.256.3", ""),
            ("v1.2.3", "-1"),
            ("v1.2.3", "4294967296"),
        ):
            with self.subTest(tag=tag, override=override), self.assertRaises(
                ValueError
            ):
                release.application_version(tag, override)

    def test_bootloader_key(self):
        source = root / "deps/chameleon/firmware/bootloader/src/dfu_public_key.c"
        array = source.read_text().split("pk[64] = {", 1)[1].split("}", 1)[0]
        public = bytes(
            int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array)
        )
        self.assertEqual(public.hex(), release.DEFAULT_PUBLIC_KEY)
        self.assertEqual(release.public_key(release.DEFAULT_SIGNING_KEY), public)

    def test_real_signing_and_rejection(self):
        with tempfile.TemporaryDirectory(prefix="tonie-release-test-") as directory:
            work = pathlib.Path(directory)
            key, other = work / "key.pem", work / "other.pem"
            for destination in (key, other):
                release.run(
                    "openssl",
                    "ecparam",
                    "-name",
                    "prime256v1",
                    "-genkey",
                    "-noout",
                    "-out",
                    str(destination),
                )
            binary = struct.pack("<II", 0x20040000, 0x27009) + b"v1.2.3\0" + b"\0" * 49
            (work / "app.bin").write_bytes(binary)
            hex_text = record(0, 4, b"\x00\x02")
            for offset in range(0, len(binary), 16):
                hex_text += record(0x7000 + offset, 0, binary[offset : offset + 16])
            (work / "app.hex").write_text(hex_text + record(0, 1, b""))
            notices = work / "notices"
            notices.mkdir()
            (notices / "LICENSE").write_text("Synthetic license notice\n")
            env = dict(
                os.environ,
                DFU_PUBLIC_KEY_HEX=release.public_key(key).hex(),
                DFU_SD_REQ="0x0100",
                DFU_APPLICATION_VERSION="",
                RUNNER_TEMP=str(work),
            )
            package = work / "release.zip"
            command = [
                sys.executable,
                str(root / "tools/release.py"),
                "sign",
                "--version",
                "v1.2.3",
                "--hex",
                str(work / "app.hex"),
                "--binary",
                str(work / "app.bin"),
                "--package",
                str(package),
                "--notices",
                str(notices),
                "--key-file",
                str(key),
            ]
            subprocess.run(command, env=env, check=True)
            public = release.public_key(key)
            init = release.verify_package(package, public, binary)
            self.assertEqual(init[1], 1)
            release.verify_package(
                package, public, require_identity=True, expected_tag="v1.2.3"
            )
            with self.assertRaisesRegex(ValueError, "different tag"):
                release.verify_package(package, public, expected_tag="v1.2.4")
            with zipfile.ZipFile(package) as archive:
                self.assertEqual(
                    archive.read("metadata/notices/LICENSE"),
                    b"Synthetic license notice\n",
                )
                identity = json.loads(archive.read("metadata/BUILD.json"))
                self.assertEqual(identity["tag"], "v1.2.3")
                self.assertEqual(identity["application_version"], 1)
                self.assertEqual(identity["hardware_version"], 0)
                self.assertFalse(
                    any(
                        key.read_bytes() in archive.read(name)
                        for name in archive.namelist()
                    )
                )
            for field, value in (
                ("sha256", "0" * 64),
                ("application_version", 2),
                ("hardware_version", 1),
                ("sd_req", "0x0101"),
                ("tag", "v1.2.4"),
                ("commit", ""),
            ):
                corrupt = work / f"corrupt-identity-{field}.zip"

                def tamper_identity(name, data):
                    if name == "metadata/BUILD.json":
                        metadata = json.loads(data)
                        metadata[field] = value
                        return json.dumps(metadata).encode()
                    return data

                rewrite_package(package, corrupt, tamper_identity)
                with self.subTest(identity_field=field), self.assertRaises(ValueError):
                    release.verify_package(corrupt, public)
            missing = work / "missing-identity.zip"
            with zipfile.ZipFile(package) as src, zipfile.ZipFile(missing, "w") as dst:
                for name in src.namelist():
                    if name != "metadata/BUILD.json":
                        dst.writestr(name, src.read(name))
            with self.assertRaisesRegex(ValueError, "identity is missing"):
                release.verify_package(missing, public, require_identity=True)
            subprocess.run(
                ["nrfutil", "nrf5sdk-tools", "pkg", "display", str(package)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
            )
            self.assertFalse(list(work.glob("tonie-sign-*")))
            with self.assertRaises(subprocess.CalledProcessError):
                release.verify_package(package, release.public_key(other))
            with self.assertRaises(ValueError):
                release.sign_package(
                    work / "app.hex",
                    work / "app.bin",
                    work / "wrong.zip",
                    other,
                    public,
                    1,
                    "0x0100",
                )
            for suffix in ("bin", "dat"):
                corrupt = work / f"corrupt-{suffix}.zip"

                def tamper(name, data):
                    if name.endswith("." + suffix):
                        data = data[:-1] + bytes((data[-1] ^ 1,))
                    return data

                rewrite_package(package, corrupt, tamper)
                with self.subTest(suffix=suffix), self.assertRaises(
                    (ValueError, subprocess.CalledProcessError)
                ):
                    release.verify_package(corrupt, public)
            for value, message in (
                (work / "missing.pem", "Signing key file is missing"),
                (other, "does not match"),
            ):
                command[-1] = str(value)
                result = subprocess.run(
                    command, env=env, capture_output=True, text=True
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(list(work.glob("tonie-sign-*")))
            env["DFU_PUBLIC_KEY_HEX"] = ""
            env.pop("DFU_SIGNING_KEY", None)
            subprocess.run(command[:-2], env=env, check=True)
            release.verify_package(
                package, bytes.fromhex(release.DEFAULT_PUBLIC_KEY), binary
            )
            with zipfile.ZipFile(package) as archive:
                self.assertFalse(
                    any(
                        release.DEFAULT_SIGNING_KEY.read_bytes() in archive.read(name)
                        for name in archive.namelist()
                    )
                )


if __name__ == "__main__":
    unittest.main()
