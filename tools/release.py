#!/usr/bin/env python3
"""Sign and verify application-only Nordic DFU releases; never access hardware."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tempfile
import zipfile

from build_version import write_version
from dfu import check_package

# Nordic little-endian X/Y public key from the pinned Chameleon bootloader.
DEFAULT_PUBLIC_KEY = (
    "7c06ab8978fc8ec5e1fe45444799b64241f2852724d4da2099966b88b4d043c6"
    "5ffca2db3f7c3630298a63715dd046c5514bc05b2ddfbf6f533e9be511efade9"
)
P256_SPKI = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d03010703420004")
DEFAULT_SIGNING_KEY = (
    pathlib.Path(__file__).resolve().parents[1]
    / "deps/chameleon/resource/dfu_key/chameleon.pem"
)


def run(*args):
    return subprocess.check_output(args, stdin=subprocess.DEVNULL, timeout=120)


def public_key(key):
    raw = run("openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER")
    if len(raw) != len(P256_SPKI) + 64 or not raw.startswith(P256_SPKI):
        raise ValueError("Signing key must use the P-256 curve")
    return raw[-64:-32][::-1] + raw[-32:][::-1]


def expected_public_key():
    raw = bytes.fromhex(os.environ.get("DFU_PUBLIC_KEY_HEX", "") or DEFAULT_PUBLIC_KEY)
    if len(raw) != 64:
        raise ValueError("DFU_PUBLIC_KEY_HEX must contain 64 Nordic public-key bytes")
    return raw


def application_version(tag, override=""):
    """Keep the upstream DFU counter independent of the displayed release tag."""
    if override:
        value = int(override, 0)
    else:
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)(?:[-+][A-Za-z0-9.+-]+)?", tag)
        if not match:
            raise ValueError("Use vMAJOR.MINOR.PATCH or set DFU_APPLICATION_VERSION")
        parts = [int(part) for part in match.groups()]
        if any(part > 255 for part in parts):
            raise ValueError("DFU version components must be 0-255")
        value = 1
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError("DFU application version must fit uint32")
    return value


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("Invalid DFU protobuf integer")


def fields(data):
    """Read the integer/byte fields used by Nordic's signed init packet."""
    result, offset = {}, 0
    while offset < len(data):
        key, offset = varint(data, offset)
        number, wire = key >> 3, key & 7
        if not number or number in result:
            raise ValueError("Invalid or duplicate DFU protobuf field")
        value, offset = varint(data, offset)
        if wire == 2:
            end = offset + value
            if end > len(data):
                raise ValueError("Truncated DFU protobuf field")
            value, offset = data[offset:end], end
        elif wire != 0:
            raise ValueError("Unsupported DFU protobuf field")
        result[number] = value
    return result


def signature_der(signature):
    if len(signature) != 64:
        raise ValueError("Invalid P-256 signature length")
    integers = []
    for raw in (signature[:32], signature[32:]):
        value = raw[::-1].lstrip(b"\0") or b"\0"
        if value[0] & 128:
            value = b"\0" + value
        integers.append(bytes((2, len(value))) + value)
    body = b"".join(integers)
    return bytes((48, len(body))) + body


def verify_package(
    path, public, expected_binary=None, *, require_identity=False, expected_tag=None
):
    check_package(path)
    with zipfile.ZipFile(path) as archive:
        app = json.loads(archive.read("manifest.json"))["manifest"]["application"]
        binary = archive.read(app["bin_file"])
        packet = fields(archive.read(app["dat_file"]))
        identity = (
            json.loads(archive.read("metadata/BUILD.json"))
            if "metadata/BUILD.json" in archive.namelist()
            else None
        )
    if set(packet) != {2}:
        raise ValueError("Release must contain a signed init packet")
    signed = fields(packet[2])
    if signed[2] != 0:
        raise ValueError("Release must use ECDSA P-256/SHA-256")
    command = fields(signed[1])
    init_bytes = command[2]
    init = fields(init_bytes)
    digest = fields(init[8])
    if (
        command[1] != 1
        or init[2] != 0
        or init[4] != 0
        or init.get(5, 0)
        or init.get(6, 0)
        or init.get(9, 0)
        or init[7] != len(binary)
        or digest[1] != 3
        or digest[2] != hashlib.sha256(binary).digest()[::-1]
    ):
        raise ValueError("Invalid application-only Ultra release metadata or hash")
    if expected_binary is not None and binary != expected_binary:
        raise ValueError("Packaged application differs from the tested build")
    if identity is None:
        if require_identity or expected_tag is not None:
            raise ValueError("Release build identity is missing")
    else:
        if not isinstance(identity, dict):
            raise ValueError("Invalid release build identity")
        tag = identity.get("tag")
        sd_req = identity.get("sd_req")
        if (
            not isinstance(tag, str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}", tag)
            or not isinstance(sd_req, str)
            or not re.fullmatch(r"0x[0-9A-Fa-f]{2,4}(?:,0x[0-9A-Fa-f]{2,4})*", sd_req)
            or not isinstance(identity.get("commit"), str)
            or not re.fullmatch(r"local|[0-9a-fA-F]{40,64}", identity["commit"])
            or type(identity.get("application_version")) is not int
            or identity["application_version"] != init[1]
            or type(identity.get("hardware_version")) is not int
            or identity["hardware_version"] != init[2]
            or identity.get("sha256") != hashlib.sha256(binary).hexdigest()
        ):
            raise ValueError(
                "Release build identity differs from the signed application"
            )
        requested = sorted({int(value, 16) for value in sd_req.split(",")})
        actual, offset = [], 0
        while offset < len(init[3]):
            value, offset = varint(init[3], offset)
            actual.append(value)
        if sorted(actual) != requested:
            raise ValueError(
                "Release build identity has different SoftDevice requirements"
            )
        if tag.encode("ascii") + b"\0" not in binary:
            raise ValueError(
                "Release tag is not embedded in the application; rebuild it"
            )
        if expected_tag is not None and tag != expected_tag:
            raise ValueError("Release build identity has a different tag")
    with tempfile.TemporaryDirectory(prefix="tonie-verify-") as directory:
        root = pathlib.Path(directory)
        pub = P256_SPKI + public[:32][::-1] + public[32:][::-1]
        (root / "public.der").write_bytes(pub)
        (root / "signature.der").write_bytes(signature_der(signed[3]))
        (root / "init.bin").write_bytes(init_bytes)
        run(
            "openssl",
            "pkey",
            "-pubin",
            "-inform",
            "DER",
            "-in",
            str(root / "public.der"),
            "-out",
            str(root / "public.pem"),
        )
        run(
            "openssl",
            "dgst",
            "-sha256",
            "-verify",
            str(root / "public.pem"),
            "-signature",
            str(root / "signature.der"),
            str(root / "init.bin"),
        )
    return init


def sign_package(hex_file, binary_file, output, key, public, version, sd_req):
    if public_key(key) != public:
        raise ValueError(
            "Signing key does not match the configured bootloader public key"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    run(
        "nrfutil",
        "nrf5sdk-tools",
        "pkg",
        "generate",
        "--hw-version",
        "0",
        "--sd-req",
        sd_req,
        "--application-version",
        str(version),
        "--application",
        str(hex_file),
        "--key-file",
        str(key),
        str(output),
    )
    init = verify_package(output, public, binary_file.read_bytes())
    requested = sorted({int(value, 16) for value in sd_req.split(",")})
    encoded = init[3]
    actual, offset = [], 0
    while offset < len(encoded):
        value, offset = varint(encoded, offset)
        actual.append(value)
    if init[1] != version or sorted(actual) != requested:
        raise ValueError("Signed DFU compatibility parameters differ from the request")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("sign", "verify"))
    parser.add_argument("--package", type=pathlib.Path, required=True)
    parser.add_argument("--version")
    parser.add_argument(
        "--hex", type=pathlib.Path, default=pathlib.Path("build/app.hex")
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("build/app.bin")
    )
    parser.add_argument("--notices", type=pathlib.Path)
    parser.add_argument(
        "--key-file",
        type=pathlib.Path,
        default=DEFAULT_SIGNING_KEY,
        help="Signing PEM; defaults to the published key in the pinned Chameleon submodule",
    )
    args = parser.parse_args()
    public = expected_public_key()
    if args.mode == "verify":
        verify_package(
            args.package, public, require_identity=True, expected_tag=args.version
        )
        print("Signed application-only Ultra DFU package verified")
        return
    if not args.key_file.is_file():
        raise ValueError(
            "Signing key file is missing; initialize submodules or use --key-file"
        )
    if not args.version:
        parser.error("sign requires --version")
    version = application_version(
        args.version, os.environ.get("DFU_APPLICATION_VERSION", "")
    )
    sd_req = os.environ.get("DFU_SD_REQ", "") or "0x0100"
    if not re.fullmatch(r"0x[0-9A-Fa-f]{2,4}(?:,0x[0-9A-Fa-f]{2,4})*", sd_req):
        raise ValueError("DFU_SD_REQ must list hexadecimal SoftDevice IDs")
    with tempfile.TemporaryDirectory(
        prefix="tonie-sign-", dir=os.environ.get("RUNNER_TEMP")
    ) as directory:
        root = pathlib.Path(directory)
        write_version(args.version, root / "version.h")
        package = root / "firmware.zip"
        sign_package(
            args.hex, args.binary, package, args.key_file, public, version, sd_req
        )
        with zipfile.ZipFile(package, "a", compression=zipfile.ZIP_DEFLATED) as archive:
            identity = {
                "tag": args.version,
                "commit": os.environ.get("GITHUB_SHA", "local"),
                "application_version": version,
                "hardware_version": 0,
                "sd_req": sd_req,
                "sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            }
            archive.writestr(
                "metadata/BUILD.json", json.dumps(identity, indent=2) + "\n"
            )
            if args.notices:
                for notice in sorted(args.notices.rglob("*")):
                    if notice.is_file():
                        archive.write(
                            notice,
                            "metadata/notices/" + str(notice.relative_to(args.notices)),
                        )
        verify_package(
            package,
            public,
            args.binary.read_bytes(),
            require_identity=True,
            expected_tag=args.version,
        )
        args.package.parent.mkdir(parents=True, exist_ok=True)
        args.package.write_bytes(package.read_bytes())
    print(f"Signed application-only DFU package: {args.package}")


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"Release command failed: {error}") from None
    except (ValueError, KeyError, IndexError) as error:
        raise SystemExit(f"Release packaging failed: {error}") from None
