/* Cross-language layouts must agree independently of either serializer. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import * as image from '../web/image-format.js';
const layout = fs.readFileSync(new URL('../src/core/image_format.h', import.meta.url), 'utf8');
const profiles = fs.readFileSync(new URL('../src/core/tag_image.h', import.meta.url), 'utf8');
function cValue(name, source = layout) {
  const match = source.match(new RegExp(`\\b${name}\\s*(?:=| )\\s*(0x[0-9a-f]+|[0-9]+)`, 'i'));
  assert(match, `Missing C constant ${name}`);
  return Number(match[1]);
}
for (const [field, offset] of Object.entries(image.IMAGE_FIELD))
  assert.equal(offset, cValue(`TAG_IMAGE_${field}_OFFSET`));
for (const [flag, mask] of Object.entries(image.IMAGE_FLAG))
  assert.equal(
    mask,
    cValue(flag === 'RESERVED' ? 'TAG_IMAGE_FLAGS_RESERVED' : `TAG_IMAGE_FLAG_${flag}`),
  );
for (const [jsName, cName] of [
  ['IMAGE_HEADER_SIZE', 'TAG_IMAGE_HEADER_SIZE'],
  ['IMAGE_MAX_BYTES', 'TAG_IMAGE_MAX_BYTES'],
  ['IMAGE_VERSION', 'TAG_IMAGE_VERSION'],
  ['IMAGE_UID_BYTES', 'TAG_IMAGE_UID_BYTES'],
  ['IMAGE_BLOCK_BYTES', 'TAG_IMAGE_BLOCK_BYTES'],
  ['IMAGE_MAX_BLOCKS', 'TAG_IMAGE_MAX_BLOCKS'],
  ['IMAGE_SLIX_L_BLOCKS', 'TAG_IMAGE_SLIX_L_BLOCKS'],
])
  assert.equal(image[jsName], cValue(cName));
assert.equal(image.IMAGE_PROFILE.SLIX, cValue('TAG_IMAGE_SLIX', profiles));
assert.equal(image.IMAGE_PROFILE.SLIX_L, cValue('TAG_IMAGE_SLIX_L', profiles));
console.log('Image layout: C and browser field offsets, flags, profiles and limits agree');
