#!/usr/bin/env bash
# Download the pinned ARM toolchain outside the source checkout; never access hardware.
set -euo pipefail

deps="${1:?Usage: install-deps.sh /absolute/external/dependency-directory}"
[[ "$deps" = /* ]] || { echo 'Dependency directory must be absolute.' >&2; exit 1; }
mkdir -p "$deps"

download() {
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 20 --max-time 600 "$1" -o "$2"
    printf '%s  %s\n' "$3" "$2" | sha256sum -c
}

download \
    'https://developer.arm.com/-/media/Files/downloads/gnu/12.3.rel1/binrel/arm-gnu-toolchain-12.3.rel1-x86_64-arm-none-eabi.tar.xz' \
    "$deps/arm.tar.xz" \
    '12a2815644318ebcceaf84beabb665d0924b6e79e21048452c5331a56332b309'
mkdir -p "$deps/arm"
tar -xJf "$deps/arm.tar.xz" --strip-components=1 -C "$deps/arm"
