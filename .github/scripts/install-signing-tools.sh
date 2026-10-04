#!/usr/bin/env bash
# Install checksum-pinned Nordic packaging tools; never access hardware.
set -euo pipefail
deps="${1:?Usage: install-signing-tools.sh /absolute/external/dependency-directory}"
[[ "$deps" = /* ]] || { echo 'Dependency directory must be absolute.' >&2; exit 1; }
mkdir -p "$deps/nrfutil"

download() {
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 20 --max-time 600 "$1" -o "$2"
    printf '%s  %s\n' "$3" "$2" | sha256sum -c
}

download \
    'https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/packages/nrfutil/nrfutil-x86_64-unknown-linux-gnu-7.12.0.tar.gz' \
    "$deps/nrfutil.tar.gz" \
    'efc18b35d075aa906556607cff0526c81d1e784b6a1f724245a6aba5dd5bf9d5'
download \
    'https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/packages/nrfutil-nrf5sdk-tools/nrfutil-nrf5sdk-tools-x86_64-unknown-linux-gnu-1.1.0.tar.gz' \
    "$deps/nrf5sdk-tools.tar.gz" \
    '84b4214c46b917696b171a692a656b0f93e7200dc93dc3ff33fafd6d88b31e4f'
tar -xzf "$deps/nrfutil.tar.gz" --strip-components=1 -C "$deps/nrfutil"
export PATH="$deps/nrfutil/data/bin:$PATH"
nrfutil install --tarball "$deps/nrf5sdk-tools.tar.gz"
nrfutil --version
nrfutil nrf5sdk-tools --version
