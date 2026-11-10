import assert from 'node:assert/strict';
import {
  importFlipper,
  exportFlipper,
  exportZip,
  safeName,
  crc32,
  inspectImage,
} from '../web/formats.js';
import fs from 'node:fs';
const hex = (a) => Array.from(a, (b) => b.toString(16).padStart(2, '0')).join(' ');
const slix = `Filetype: Flipper NFC device
Version: 4
Device type: SLIX
UID: E0 04 03 02 01 00 00 01
Block Size: 04
Block Count: 8
Data Content: ${hex(new Uint8Array(32).map((_, i) => i))}
Security Status: 00 00 00 00 00 00 00 01
Password Privacy: 01 02 03 04
Privacy Mode: true
`;
const image = importFlipper(slix);
assert.equal(image[5], 2);
const original = exportFlipper(image);
assert.match(original, /^Device type: SLIX$/m);
assert.match(original, /^Capabilities: Default$/m);
assert.deepEqual(importFlipper(original), image);
assert.throws(() => importFlipper(slix + 'UID: 01\n'));
assert.throws(() => importFlipper(slix.replace('Block Count: 8', 'Block Count: 9')));
assert.throws(() => importFlipper(slix.replace('SLIX', 'Mifare Classic')));
assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
assert.ok(new TextEncoder().encode(safeName('é'.repeat(90))).length <= 63);
fs.writeFileSync('build/test-slix.nfci', image);
fs.writeFileSync('build/test-slix.nfc', original);
const full = importFlipper(
  slix
    .replace('Block Count: 8', 'Block Count: 28')
    .replace(/Data Content: [^\n]+/, `Data Content: ${hex(new Uint8Array(112))}`)
    .replace(/Security Status: [^\n]+/, `Security Status: ${hex(new Uint8Array(28))}`),
);
assert.equal(full[5], 1);
assert.deepEqual(importFlipper(exportFlipper(full)), full);
const unsupported = image.slice();
unsupported[5] = 3;
assert.throws(() => exportFlipper(unsupported));
assert.deepEqual(inspectImage(image), {
  uid: 'E004030201000001',
  profile: 'SLIX-L',
  privacy: true,
});
const zip = exportZip([{ name: 'Synthetic.nfc', text: slix }]);
fs.writeFileSync('build/test-export.zip', new Uint8Array(await zip.arrayBuffer()));
console.log(
  'Formats: SLIX/SLIX-L round trips, geometry/unsupported families/duplicates, UTF-8 and ZIP passed',
);
