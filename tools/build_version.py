#!/usr/bin/env python3
"""Generate the firmware identity from the release tag or local build version."""

import os
import pathlib
import re


def write_version(version, destination):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}", version):
        raise ValueError(
            "Firmware version must be 1-64 letters, digits, '.', '_', '+' or '-'"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(f'#pragma once\n#define FIRMWARE_VERSION "{version}"\n')


if __name__ == "__main__":
    write_version(
        os.environ.get("FIRMWARE_VERSION", "0.1.0-dev"),
        pathlib.Path(__file__).resolve().parents[1] / "build/firmware_version.h",
    )
