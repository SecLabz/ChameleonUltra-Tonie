# Development

## Build and test

Install Python 3, Node.js, a C11 compiler with ASan/UBSan, and ARM GCC/newlib.
Initialize the pinned submodules, then run:

```sh
git submodule sync
git submodule update --init
make test
make app
```

`make app` builds application ELF/HEX/BIN files without flashing. Updates keep
the installed bootloader, MBR, and SoftDevice. Release tests check the default
DFU key against the upstream bootloader source.

Use pinned commits, not `git submodule update --remote`. Optional `SDK`,
`TINYUSB`, `LWIP`, and `ARM_PREFIX` overrides go in ignored `.local.env`.

Without embedded dependencies: `make test-core test-phy test-controls test-app test-web test-tools`.
The full suite also tests USB descriptors and RNDIS control messages using
the pinned TinyUSB headers, DHCP with lwIP, and HTTP on localhost:8080; stop an
existing simulator first. Tests use simulated RF/flash and never contact USB hardware.

## Try locally

```sh
make host
build/tonie-host build/demo-library.bin
```

Open [127.0.0.1:8080](http://127.0.0.1:8080); stop with Ctrl-C.
RF/flash are simulated, battery is fixed at 83%, and DFU only logs a message.
Reusing the file preserves the collection, but `make clean` deletes `build/`.
Keep real backups, recovery packages, keys, and RF captures outside it;
ignored `private/` survives clean. Only one simulator can use port 8080.

## Architecture

The main loop owns USB/network processing, flash mutations, and profile changes:

```text
Embedded gzip page → /api/v1 → application/library
                             → NFC-V session → SLIX engine → GPIO PHY
```

TinyUSB supplies two mutually exclusive USB configurations: RNDIS first for
Windows, CDC-ECM second for ECM hosts. Both share endpoints and the same lwIP
interface, DHCP server, HTTP API, and address. Microsoft OS 1.0 descriptors
advertise the built-in RNDIS driver. The application PID is `cafe:4061` (formerly
ECM-only `cafe:4060`) to avoid reusing cached driver bindings; the DFU identity
is unchanged. The RNDIS handler is adapted from TinyUSB's pinned implementation, checks
request lengths against its 120-byte buffer, and reports the same host adapter
MAC as the ECM descriptor. The lwIP device MAC differs in its final bit.

Run `make test-usb` for host-only descriptor, driver-association, MAC, RNDIS
initialization/query/filter/reset/keepalive, and malformed-request checks.
These checks cannot establish Windows driver recognition. Authorized hardware
validation must check automatic enumeration and DHCP, opening `192.168.7.1`,
and unplug/replug with each transport; on Windows 10/11, verify the built-in
RNDIS driver loads without a custom INF. Linux can exercise both configurations
but cannot validate Windows binding. Keep the physical DFU recovery route and
a working signed application package available when testing USB changes.

RNDIS DHCP, HTTP, and DFU entry were validated on Linux with `rndis_host`;
web access on Windows 11 was reported working with `v0.1.0-ecm-rndis.2`.
Windows 10, the new layout's ECM configuration, and iPhone still need hardware
validation. In a VM, attach the application (`cafe:4061`) or DFU (`1915:521f`)
to the intended OS; Windows cannot use an adapter passed through to Linux.

lwIP uses raw TCP. The page, fonts, and notices are
local; gzip is served directly from flash. Keep the unauthenticated
[HTTP API](#http-api) on its USB link: tag passwords are plaintext in flash.

| Path                                      | Responsibility                                       |
| ----------------------------------------- | ---------------------------------------------------- |
| `src/common`, `src/core`                  | Bytes/CRC, images, journal, application, controls    |
| `src/tag`                                 | Image loading, emulation, session ownership          |
| `src/tag/protocol/nfcv`                   | Frames, CRC, commands, UID wire order                |
| `src/tag/phy/nfcv`                        | PHY contract, PPM, timing, diagnostics               |
| `src/tag/profiles/nfcv`, `src/cards/slix` | Standard commands, memory, SLIX privacy/password/EAS |
| `src/platform/ultra`                      | Physical GPIO RF, board controls, USB                |
| `src/net`, `src/config`, `src/host`       | HTTP/network, layout, simulator                      |
| `web`, `tools`, `tests`                   | Interface, build/update tools, local checks          |

Stored originals and active RF RAM are separate. Reader writes are never saved
automatically, including on standby. Flash mutations stop RF; only selection
can be persisted independently of reader writes. `FNB2` has two 96 KiB
CRC-protected journal banks, commits records/compaction last, and preserves the
ID high-water mark. Recovery stops at a torn tail and rejects corrupt committed
headers or incompatible formats without automatic migration/erase.
[Update boundaries](updates.md).

`nfcv_emulation_start_from_image()` loads the original, configures the session,
and starts the PHY. Only the PHY owns detector state and antenna switching;
`app_state.loaded_original` is the reload snapshot. Follow `.clang-format`,
`.prettierrc.json`, `.editorconfig`, and standard Black formatting for Python.

## HTTP API

The browser and tools share `/api/v1/` at `http://192.168.7.1` on the Ultra
or `http://127.0.0.1:8080` in the simulator. `GET state` and `GET tags` return
JSON; `GET tag?id=N` exports the saved NFCI image. Mutations use POST;
`import?name=X` takes NFCI bytes as `application/octet-stream`, not `.nfc` text.
URL-encode query values. Bodies are limited to 1024 bytes and complete requests
to 2048 bytes. See [API routes](../src/net/api.c) and
[HTTP validation](../src/net/http.c) for the full contract.

Library edits pause RF; `emulate?id=N` reloads the original and resumes it.
A busy reader field rejects activation. Reader writes only affect active RAM.

NFCI v1 (NFC Image) uses a 128-byte header, raw memory, then one lock byte (0 or 1) per
four-byte block. Multibyte values and password words are little-endian; the
eight-byte UID is in display order. Bytes 26–27 and 88–127 must be zero;
28–67 are ignored. SLIX-L has eight blocks; the maximum is 160.
See [field offsets](../src/core/image_format.h),
[validation](../src/core/tag_image.c), and [Flipper conversion](../web/formats.js).
Exports retain supported memory, UID, locks, and passwords, not all Flipper fields.

`NFCV_DIAGNOSTICS` builds start paused and expose bring-up routes in the API
source. Each request renews their 90-second control lease. Use synthetic tags:
RF traces may contain passwords. Keep captures outside Git and `build/`.
Counters do not establish RF waveforms or hardware compatibility.

## RF timing

Keep the [NFC-V mechanism](nfcv.md) and the established timing in
[the GPIO PHY](../src/platform/ultra/nfcv_gpio.c),
[timing definitions](../src/tag/phy/nfcv/timing.h), and
[PPM macros](../src/tag/phy/nfcv/ppm.h):

- Preserve detector thresholds, integer arithmetic, polling order, and RX/TX lifecycle.
- RX, TX, and waveform preparation run from RAM with interrupts masked in bounded RF windows. Keep preparation inline in its RAM caller; USB runs between windows.
- Schedule 423.75 kHz with rational 75/76-cycle half-period deadlines at 64 MHz. Sample lateness after GPIO writes. Active segments end high; finish the last interval before restoring the switch and NFCT.
- Waveform start is detector EOF + `604 + 20540 - 32 = 21112` cycles (329.875 µs). The initial 24 inactive subcarrier cycles add about 56.64 µs before the first high-rate burst. These are not independent antenna-edge measurements.
- PPM expressions expand in the receiver; macro arguments must be scalars without side effects. Even forced-inline function extraction changed register allocation with the validated compiler.

Inspect generated instructions for timing-path changes. Local tests/counters
cannot establish RF compatibility. Hardware tests require explicit authorization.
Before remote firmware changes, verify recovery and retain a working fallback.
Never replace the bootloader without an independently verified recovery route.

## Releases

[CI](../.github/workflows/ci.yml) tests pinned sources and production builds,
then signs/verifies application-only DFU packages. Branch/PR builds show
`0.1.0-dev`; manual dispatch checks without publication. For a local package:

```sh
make test app FIRMWARE_VERSION=v0.1.0
python3 tests/release.py
python3 tools/release.py sign --version v0.1.0 \
  --package build/ChameleonUltra-Tonie-v0.1.0-dfu.zip
python3 tools/release.py verify --version v0.1.0 \
  --package build/ChameleonUltra-Tonie-v0.1.0-dfu.zip
```

Signing needs nrfutil (`nrf5sdk-tools`) and OpenSSL. `make app` is unsigned.
Use the same version for build and signing. Add `--notices DIR` for local
notices; CI collects them automatically. Push the source/workflow before
publishing an annotated tag:

```sh
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin v0.1.0
```

Pushed `v*` tags publish a DFU ZIP named after the release: `v1.0.0` publishes
`ChameleonUltra-Tonie-v1.0.0-dfu.zip`. Use new tags for
fixes; published releases are never overwritten and published tags must not move.
[Install a release](updates.md).

<details>
<summary>Signing, version rules, and CI configuration</summary>

Defaults: published `deps/chameleon/resource/dfu_key/chameleon.pem`, Ultra
hardware version `0`, S140 7.2.0 (`0x0100`). CI checks that the default
signing key matches the public key in the upstream ChameleonUltra
bootloader source. The public signing key provides compatibility, not exclusive
authorship. PEM files never enter the ZIP. Keep custom keys external; use
`--key-file` and update CI's explicit signing input for a custom bootloader.

| Actions variable          | Meaning/default                                                         |
| ------------------------- | ----------------------------------------------------------------------- |
| `DFU_PUBLIC_KEY_HEX`      | 128 hex digits, Nordic little-endian X/Y; upstream default |
| `DFU_SD_REQ`              | Comma-separated installed SoftDevice IDs; default `0x0100`              |
| `DFU_APPLICATION_VERSION` | Optional uint32 override, decimal or hex; default `1`, like upstream    |

Prefer `vMAJOR.MINOR.PATCH` with components 0–255 and optional prerelease/build
suffix. The displayed version follows the tag; the DFU application version stays
at `1`, like upstream, so the official bootloader can accept either firmware.
An explicit override permits other supported `v*` names or a custom counter.
A hyphen marks a prerelease and excludes it from Latest.

CI ZIPs contain the application, signed init packet, manifest, `metadata/BUILD.json`,
and notices. Only the application is flashed. Verification checks signatures,
hashes, DFU compatibility, bounds, build metadata, and the requested tag.
Unsigned/debug DFU, non-Ultra, bootloader/SoftDevice, and oversized packages
are rejected. Build metadata and notices are not signed.
Publication verifies again. Signed artifacts last 14 days; ELF/MAP and unsigned
firmware are not uploaded. Source archives omit submodules; initialize them
after checking out the release tag.

CI checksum-verifies external tools, uses `contents: read` for builds and
`contents: write` only for publication with `GITHUB_TOKEN`. No signing secret
or personal token is needed. Enable Actions and allow the pinned
checkout/setup-node/upload/download actions. CI adds no hardware validation.

</details>

<details>
<summary>Build inputs and licenses</summary>

## Build inputs and licenses

Dependencies are pinned in [.gitmodules](../.gitmodules) and Git: official
Chameleon nRF5 SDK 17.1.0 with its NFCT changes, TinyUSB 0.17.0 (MIT),
lwIP 2.2.0 (BSD), and GNU Arm 12.3.rel1.

Local `make` uses installed dependencies. CI downloads checksum-pinned ARM
and Nordic tools: nrfutil 7.12.0 / nrf5sdk-tools 1.1.0, with OpenSSL for verification.

Preserve Nordic/nrfx/CMSIS/MDK, CodeSourcery, and
[font license notices](../web/fonts/PROVENANCE.txt). CI ZIPs include dependency,
tool, font, and linker notices, including USB networking.
This repository starts from a source snapshot; earlier history is not included.

</details>

[Project README](../README.md)
