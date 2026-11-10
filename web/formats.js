import {
  IMAGE_HEADER_SIZE as HEADER,
  IMAGE_MAX_BYTES as MAX_IMAGE,
  IMAGE_VERSION,
  IMAGE_UID_BYTES,
  IMAGE_BLOCK_BYTES,
  IMAGE_MAX_BLOCKS,
  IMAGE_SLIX_L_BLOCKS,
  IMAGE_PROFILE,
  IMAGE_FIELD,
  IMAGE_FLAG,
} from './image-format.js';
/* Flipper conversion runs entirely in the browser. */
const encoder = new TextEncoder();

export function crc32(bytes) {
  let c = 0xffffffff;
  for (const b of bytes) {
    c ^= b;
    for (let i = 0; i < 8; i++) c = (c >>> 1) ^ (c & 1 ? 0xedb88320 : 0);
  }
  return (c ^ 0xffffffff) >>> 0;
}
function hex(s, n, label) {
  const words = s.trim().split(/\s+/);
  if (words.length !== n || words.some((x) => !/^[0-9a-f]{2}$/i.test(x)))
    throw Error(`${label}: expected ${n} hexadecimal bytes`);
  return Uint8Array.from(words, (x) => parseInt(x, 16));
}
function hexText(a) {
  return Array.from(a, (x) => x.toString(16).padStart(2, '0').toUpperCase()).join(' ');
}
function integer(s, label, base = 10) {
  if (!s || !(base === 16 ? /^[0-9a-f]+$/i : /^\d+$/).test(s))
    throw Error(`${label}: invalid number`);
  return parseInt(s, base);
}
function flag(fields, k) {
  if (!(k in fields)) return false;
  if (!/^(true|false)$/i.test(fields[k])) throw Error(`${k}: invalid boolean`);
  return fields[k].toLowerCase() === 'true';
}
export function safeName(s) {
  let name =
    s
      .replace(/\.nfc$/i, '')
      .trim()
      .replace(/[\x00-\x1f/\\]/g, '_') || 'Untitled';
  while (encoder.encode(name).length > 63) name = Array.from(name).slice(0, -1).join('');
  return name;
}
function validateImage(b) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (
    b.length < HEADER ||
    b.length > MAX_IMAGE ||
    String.fromCharCode(...b.slice(0, 4)) !== 'NFCI' ||
    b[IMAGE_FIELD.VERSION] !== IMAGE_VERSION
  )
    throw Error('Unsupported tag image');
  const count = v.getUint16(IMAGE_FIELD.BLOCK_COUNT, true),
    dataSize = v.getUint16(IMAGE_FIELD.DATA_SIZE, true),
    lockCount = v.getUint16(IMAGE_FIELD.LOCK_SIZE, true);
  if (
    HEADER + dataSize + lockCount !== b.length ||
    b[IMAGE_FIELD.BLOCK_SIZE] !== IMAGE_BLOCK_BYTES ||
    b[IMAGE_FIELD.FLAGS] & IMAGE_FLAG.RESERVED ||
    b[IMAGE_FIELD.RESERVED_GEOMETRY] ||
    b[IMAGE_FIELD.RESERVED_GEOMETRY + 1] ||
    b.slice(IMAGE_FIELD.RESERVED_TAIL, HEADER).some(Boolean)
  )
    throw Error('Invalid tag geometry');
  if (
    b[IMAGE_FIELD.PROFILE] === IMAGE_PROFILE.SLIX ||
    b[IMAGE_FIELD.PROFILE] === IMAGE_PROFILE.SLIX_L
  ) {
    if (
      b[IMAGE_FIELD.UID_SIZE] !== IMAGE_UID_BYTES ||
      b[IMAGE_FIELD.UID] !== 0xe0 ||
      count < 1 ||
      count > IMAGE_MAX_BLOCKS ||
      dataSize !== count * IMAGE_BLOCK_BYTES ||
      lockCount !== count ||
      (b[IMAGE_FIELD.PROFILE] === IMAGE_PROFILE.SLIX_L && count !== IMAGE_SLIX_L_BLOCKS) ||
      b.subarray(HEADER + dataSize).some((x) => x > 1)
    )
      throw Error('Invalid SLIX image');
  } else throw Error('Tag family is not supported');
  return b;
}
export function importFlipper(text) {
  const fields = {};
  for (const line of text.replace(/^\uFEFF/, '').split(/\r?\n/)) {
    if (!line.trim() || line.trim().startsWith('#')) continue;
    const at = line.indexOf(':');
    if (at < 0) continue;
    const k = line.slice(0, at).trim();
    if (k in fields) throw Error(`Duplicate field: ${k}`);
    fields[k] = line.slice(at + 1).trim();
  }
  if (fields.Filetype !== 'Flipper NFC device') throw Error('This is not a Flipper NFC file');
  if (fields.Version && !['3', '4'].includes(fields.Version))
    throw Error('Unsupported Flipper NFC version');
  if (!/^(SLIX|SLIX-L)$/.test(fields['Device type'] || ''))
    throw Error('Only SLIX and SLIX-L are supported');
  const count = integer(fields['Block Count'], 'Block Count');
  if (count < 1 || count > IMAGE_MAX_BLOCKS) throw Error('Unsupported block count');
  if (integer(fields['Block Size'], 'Block Size', 16) !== IMAGE_BLOCK_BYTES)
    throw Error('Only four-byte blocks are supported');
  const b = new Uint8Array(HEADER + count * IMAGE_BLOCK_BYTES + count),
    v = new DataView(b.buffer);
  b.set(encoder.encode('NFCI'));
  b[IMAGE_FIELD.VERSION] = IMAGE_VERSION;
  b[IMAGE_FIELD.PROFILE] =
    count === IMAGE_SLIX_L_BLOCKS ? IMAGE_PROFILE.SLIX_L : IMAGE_PROFILE.SLIX;
  b[IMAGE_FIELD.UID_SIZE] = IMAGE_UID_BYTES;
  b.set(hex(fields.UID || '', b[IMAGE_FIELD.UID_SIZE], 'UID'), IMAGE_FIELD.UID);
  b[IMAGE_FIELD.BLOCK_SIZE] = IMAGE_BLOCK_BYTES;
  v.setUint16(IMAGE_FIELD.BLOCK_COUNT, count, true);
  v.setUint16(IMAGE_FIELD.DATA_SIZE, count * IMAGE_BLOCK_BYTES, true);
  v.setUint16(IMAGE_FIELD.LOCK_SIZE, count, true);
  for (const [key, offset] of [
    ['DSFID', IMAGE_FIELD.DSFID],
    ['AFI', IMAGE_FIELD.AFI],
    ['IC Reference', IMAGE_FIELD.IC_REF],
  ])
    b[offset] = hex(fields[key] || '00', 1, key)[0];
  b[IMAGE_FIELD.FLAGS] =
    (flag(fields, 'Privacy Mode') ? IMAGE_FLAG.PRIVACY : 0) |
    (flag(fields, 'Lock DSFID') ? IMAGE_FLAG.DSFID_LOCKED : 0) |
    (flag(fields, 'Lock AFI') ? IMAGE_FLAG.AFI_LOCKED : 0) |
    (flag(fields, 'Lock EAS') ? IMAGE_FLAG.EAS_LOCKED : 0);
  b.set(hex(fields['Data Content'] || '', count * IMAGE_BLOCK_BYTES, 'Data Content'), HEADER);
  b.set(
    hex(fields['Security Status'] || '', count, 'Security Status'),
    HEADER + count * IMAGE_BLOCK_BYTES,
  );
  for (const [i, key] of ['Read', 'Write', 'Privacy', 'Destroy', 'EAS'].entries())
    if (fields[`Password ${key}`])
      b.set(
        hex(fields[`Password ${key}`], 4, `Password ${key}`).reverse(),
        IMAGE_FIELD.PASSWORDS + i * 4,
      );
  return validateImage(b);
}
export function inspectImage(bytes) {
  const b = validateImage(bytes),
    uid = Array.from(b.subarray(IMAGE_FIELD.UID, IMAGE_FIELD.UID + b[IMAGE_FIELD.UID_SIZE]), (x) =>
      x.toString(16).padStart(2, '0').toUpperCase(),
    ).join('');
  return {
    uid,
    profile: b[IMAGE_FIELD.PROFILE] === IMAGE_PROFILE.SLIX_L ? 'SLIX-L' : 'SLIX',
    privacy: !!(b[IMAGE_FIELD.FLAGS] & IMAGE_FLAG.PRIVACY),
  };
}
export function exportFlipper(bytes) {
  const b = validateImage(bytes),
    v = new DataView(b.buffer, b.byteOffset, b.byteLength),
    count = v.getUint16(IMAGE_FIELD.BLOCK_COUNT, true),
    m = b.subarray(HEADER, HEADER + count * IMAGE_BLOCK_BYTES);
  const lines = [
    'Filetype: Flipper NFC device',
    'Version: 4',
    'Device type: SLIX',
    `UID: ${hexText(b.subarray(IMAGE_FIELD.UID, IMAGE_FIELD.UID + b[IMAGE_FIELD.UID_SIZE]))}`,
    `DSFID: ${hexText(b.subarray(IMAGE_FIELD.DSFID, IMAGE_FIELD.DSFID + 1))}`,
    `AFI: ${hexText(b.subarray(IMAGE_FIELD.AFI, IMAGE_FIELD.AFI + 1))}`,
    `IC Reference: ${hexText(b.subarray(IMAGE_FIELD.IC_REF, IMAGE_FIELD.IC_REF + 1))}`,
    `Lock DSFID: ${!!(b[IMAGE_FIELD.FLAGS] & IMAGE_FLAG.DSFID_LOCKED)}`,
    `Lock AFI: ${!!(b[IMAGE_FIELD.FLAGS] & IMAGE_FLAG.AFI_LOCKED)}`,
    `Block Count: ${count}`,
    'Block Size: 04',
    `Data Content: ${hexText(m)}`,
    `Security Status: ${hexText(b.subarray(HEADER + count * IMAGE_BLOCK_BYTES))}`,
    'Capabilities: Default',
  ];
  for (const [i, key] of ['Read', 'Write', 'Privacy', 'Destroy', 'EAS'].entries()) {
    const offset = IMAGE_FIELD.PASSWORDS + i * 4;
    const password = b.slice(offset, offset + 4);
    if (i >= 2 || password.some(Boolean))
      lines.push(`Password ${key}: ${hexText(password.reverse())}`);
  }
  lines.push(
    `Privacy Mode: ${!!(b[IMAGE_FIELD.FLAGS] & IMAGE_FLAG.PRIVACY)}`,
    `Lock EAS: ${!!(b[IMAGE_FIELD.FLAGS] & IMAGE_FLAG.EAS_LOCKED)}`,
  );
  return lines.join('\n') + '\n';
}
/* Small uncompressed ZIP writer; NFC text files are tiny, no ZIP dependency. */
export function exportZip(files) {
  const chunks = [],
    central = [];
  let offset = 0,
    total = 0;
  for (const file of files) {
    const name = encoder.encode(file.name),
      data = encoder.encode(file.text),
      crc = crc32(data),
      h = new Uint8Array(30 + name.length),
      v = new DataView(h.buffer);
    v.setUint32(0, 0x04034b50, true);
    v.setUint16(4, 20, true);
    v.setUint16(6, 0x800, true);
    v.setUint32(14, crc, true);
    v.setUint32(18, data.length, true);
    v.setUint32(22, data.length, true);
    v.setUint16(26, name.length, true);
    h.set(name, 30);
    chunks.push(h, data);
    const c = new Uint8Array(46 + name.length),
      cv = new DataView(c.buffer);
    cv.setUint32(0, 0x02014b50, true);
    cv.setUint16(4, 20, true);
    cv.setUint16(6, 20, true);
    cv.setUint16(8, 0x800, true);
    cv.setUint32(16, crc, true);
    cv.setUint32(20, data.length, true);
    cv.setUint32(24, data.length, true);
    cv.setUint16(28, name.length, true);
    cv.setUint32(42, offset, true);
    c.set(name, 46);
    central.push(c);
    total += c.length;
    offset += h.length + data.length;
  }
  const end = new Uint8Array(22),
    v = new DataView(end.buffer);
  v.setUint32(0, 0x06054b50, true);
  v.setUint16(8, files.length, true);
  v.setUint16(10, files.length, true);
  v.setUint32(12, total, true);
  v.setUint32(16, offset, true);
  return new Blob([...chunks, ...central, end], { type: 'application/zip' });
}
