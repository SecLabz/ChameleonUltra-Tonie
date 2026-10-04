# Updates and recovery

Use the signed **application-only** DFU ZIP from this project's GitHub Releases,
such as `ChameleonUltra-Tonie-v1.0.0-dfu.zip` for version `v1.0.0`.
Updates use the installed Nordic DFU bootloader
and preserve it, the MBR, and the SoftDevice.

## Before updating

- Use **Export all** and record favorites separately. Restore tags by extracting the backup ZIP and importing its `.nfc` files.
- Keep backups and a working signed application package outside `build/`. The [official Ultra application](https://github.com/RfidResearchGroup/ChameleonUltra/releases) is a fallback.
- Match the signing key and SoftDevice requirements to the installed loader. Before remote changes, independently verify usable remote DFU recovery; an HTTP restart alone is insufficient. Never replace the bootloader without an independently verified recovery route.

## Browser update

For initial installation, follow the [README](../README.md#install-from-the-official-chameleon-ultra-firmware).
For later updates:

1. Open **Device → Restart in DFU mode**. If the page is unavailable, enter DFU manually: with the official loader, disconnect USB, wait for sleep, hold **B** while reconnecting, then release it. See the [official guide](https://github.com/RfidResearchGroup/ChameleonUltra/wiki/firmware#the-bootloader) for timing and alternatives.
2. In desktop Chrome, open the [DFU flasher](https://taichunmin.idv.tw/chameleon-ultra.js/dfu.html), choose **USB Serial (PC only)**, and load the ZIP under **From Zip File**. The GitHub selector targets upstream firmware.
3. Click **Update selected**, select the Ultra's DFU port, and wait for restart. If DFU times out, enter it again.
4. Reopen [192.168.7.1](http://192.168.7.1). Check the version, originals, names, selection, and favorites. The selected tag starts on reboot; pause if needed.

If your Ultra has a different bootloader, follow its documentation for DFU
entry and verify that it accepts the signed Nordic DFU application ZIP.
Browser installation still needs hardware validation.

## CLI fallback

Install [nrfutil](https://docs.nordicsemi.com/r/bundle/nrfutil/page/guides/installing.html)
and its device command, then enter DFU and identify the device:

```sh
nrfutil install device
nrfutil device list --traits nordicDfu
python3 tools/dfu.py program --serial YOUR_DFU_SERIAL \
  --package /path/to/ChameleonUltra-Tonie-v1.0.0-dfu.zip
```

Use the DFU serial (`1915:521F`), which differs from the USB Ethernet application's
serial. The helper checks device identity and application bounds and rejects
bootloader/SoftDevice components. Nordic DFU checks signature and compatibility.

## Storage and recovery

Tonie updates stay within `0x27000–0xC3000` (end exclusive), leaving the 192 KiB
library at `0xC3000–0xF3000` untouched. Other firmware may overwrite it; export
before switching. See [flash layout](../src/config/flash_layout.h).

If the library is intact, avoid importing duplicates. An incompatible journal
is never migrated or erased automatically; **Erase library** is destructive.
If a package is rejected, check its key and compatibility requirements.

[Build/sign a package](development.md#releases) · [Project README](../README.md)
