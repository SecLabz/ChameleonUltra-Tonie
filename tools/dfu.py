#!/usr/bin/env python3
"""Enter the existing DFU bootloader and program application-only packages."""

import argparse
import json
import pathlib
import struct
import subprocess
import time
import urllib.request
import zipfile

APP_START = 0x27000
APP_END = 0xC3000


def check_package(path):
    """Check the archive, application window and Cortex-M entry vectors."""
    with zipfile.ZipFile(path) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError("Duplicate archive entries")
        if archive.testzip() is not None:
            raise ValueError("Corrupt archive")
        manifest = json.loads(archive.read("manifest.json"))["manifest"]
        if set(manifest) != {"application"}:
            raise ValueError("Only application packages are supported")
        app = manifest["application"]
        archive.getinfo(app["dat_file"])
        size = archive.getinfo(app["bin_file"]).file_size
        if size < 8 or size > APP_END - APP_START:
            raise ValueError("Application is empty or overlaps the library")
        binary = archive.read(app["bin_file"])
        stack, entry = struct.unpack_from("<II", binary)
        if stack & 7 or not 0x20000000 < stack <= 0x20040000:
            raise ValueError("Invalid application stack pointer")
        if not entry & 1 or not APP_START <= entry - 1 < APP_START + size:
            raise ValueError("Invalid application entry point")
        return size


def inventory():
    result = subprocess.run(
        ["nrfutil", "--json", "device", "list"],
        capture_output=True,
        text=True,
        check=True,
        timeout=15,
    )
    for line in reversed(result.stdout.splitlines()):
        data = json.loads(line).get("data", {})
        if "devices" in data:
            return data["devices"]
    raise RuntimeError("No USB inventory")


def target(serial):
    matches = [device for device in inventory() if device["serialNumber"] == serial]
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one device with the selected DFU serial")
    device = matches[0]
    descriptor = device.get("usb", {}).get("device", {}).get("descriptor", {})
    if (descriptor.get("idVendor"), descriptor.get("idProduct")) != (0x1915, 0x521F):
        raise RuntimeError("Selected device is not the existing Nordic DFU interface")
    if not device.get("traits", {}).get("nordicDfu"):
        raise RuntimeError("Selected device does not support Nordic DFU")
    return device


def enter(serial):
    request = urllib.request.Request(
        "http://192.168.7.1/api/v1/bootloader",
        data=b"",
        headers={"Content-Type": "application/octet-stream"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=8) as response:
        if not json.load(response).get("ok"):
            raise RuntimeError("Application refused DFU entry")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            target(serial)
            print("Selected DFU interface confirmed")
            return
        except RuntimeError:
            time.sleep(0.5)
    raise RuntimeError("Selected DFU interface did not appear; do not program")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["inventory", "enter", "program"])
    parser.add_argument(
        "--serial", required=True, help="Serial of the existing DFU bootloader"
    )
    parser.add_argument("--package", type=pathlib.Path)
    args = parser.parse_args()
    if args.mode == "enter":
        enter(args.serial)
    elif args.mode == "inventory":
        print(json.dumps(target(args.serial), indent=2))
    else:
        if not args.package:
            parser.error("program requires --package")
        size = check_package(args.package)
        target(args.serial)
        print(f"Programming application only: {size} bytes", flush=True)
        subprocess.run(
            [
                "nrfutil",
                "device",
                "program",
                "--firmware",
                str(args.package.resolve()),
                "--serial-number",
                args.serial,
            ],
            check=True,
            timeout=90,
        )


if __name__ == "__main__":
    main()
