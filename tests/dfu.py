"""DFU package checks use synthetic archives and never access USB hardware."""

import importlib.util
import json
import pathlib
import struct
import tempfile
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dfu", root / "tools/dfu.py")
dfu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dfu)


def package(path, binary, extra=None, init=True):
    manifest = {"application": {"bin_file": "app.bin", "dat_file": "app.dat"}}
    if extra:
        manifest[extra] = {}
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("manifest.json", json.dumps({"manifest": manifest}))
        archive.writestr("app.bin", binary)
        if init:
            archive.writestr("app.dat", b"synthetic init packet")


def rejected(path):
    try:
        dfu.check_package(path)
    except (ValueError, KeyError, zipfile.BadZipFile):
        return
    raise AssertionError("Unsafe or malformed package was accepted")


with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / "app.zip"
    binary = struct.pack("<II", 0x20040000, dfu.APP_START + 9) + b"\x00" * 8
    package(path, binary)
    assert dfu.check_package(path) == len(binary)
    for component in ["bootloader", "softdevice", "softdevice_bootloader"]:
        package(path, binary, extra=component)
        rejected(path)
    for stack, entry in [
        (0x20040001, dfu.APP_START + 9),
        (0x20000000, dfu.APP_START + 9),
        (0x20040000, dfu.APP_START + 8),
        (0x20040000, dfu.APP_START - 1),
        (0x20040000, dfu.APP_END + 1),
    ]:
        package(path, struct.pack("<II", stack, entry) + b"\x00" * 8)
        rejected(path)
    for invalid in [b"", binary[:7], binary + b"\x00" * (dfu.APP_END - dfu.APP_START)]:
        package(path, invalid)
        rejected(path)
    package(path, binary, init=False)
    rejected(path)
    path.write_bytes(b"not a ZIP")
    rejected(path)
# Device selection must fail closed before any programming command.
device = {
    "serialNumber": "SYNTHETIC",
    "traits": {"nordicDfu": True},
    "usb": {"device": {"descriptor": {"idVendor": 0x1915, "idProduct": 0x521F}}},
}
dfu.inventory = lambda: [device]
assert dfu.target("SYNTHETIC") is device
for devices, serial in [
    ([], "SYNTHETIC"),
    ([device], "OTHER"),
    ([device, device], "SYNTHETIC"),
]:
    dfu.inventory = lambda: devices
    try:
        dfu.target(serial)
    except RuntimeError:
        continue
    raise AssertionError("Ambiguous or wrong serial was accepted")
dfu.inventory = lambda: [device]
for descriptor, traits in [
    ({"idVendor": 0x1915, "idProduct": 0x521F}, {}),
    ({"idVendor": 0xCAFE, "idProduct": 0x4060}, {"nordicDfu": True}),
]:
    device["usb"]["device"]["descriptor"] = descriptor
    device["traits"] = traits
    try:
        dfu.target("SYNTHETIC")
    except RuntimeError:
        continue
    raise AssertionError("Non-DFU target was accepted")
print(
    "DFU: package bounds, entry vectors, malformed archives and exact DFU selection passed"
)
