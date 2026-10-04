/* NFCI v1 fields mirror src/core/image_format.h; checked by image-layout.mjs. */
export const IMAGE_HEADER_SIZE = 128;
export const IMAGE_MAX_BYTES = 1024;
export const IMAGE_VERSION = 1;
export const IMAGE_UID_BYTES = 8;
export const IMAGE_BLOCK_BYTES = 4;
export const IMAGE_MAX_BLOCKS = 160;
export const IMAGE_SLIX_L_BLOCKS = 8;
export const IMAGE_PROFILE = { SLIX: 1, SLIX_L: 2 };
export const IMAGE_FIELD = {
  VERSION: 4,
  PROFILE: 5,
  UID_SIZE: 6,
  FLAGS: 7,
  UID: 8,
  DSFID: 16,
  AFI: 17,
  IC_REF: 18,
  BLOCK_SIZE: 19,
  BLOCK_COUNT: 20,
  DATA_SIZE: 22,
  LOCK_SIZE: 24,
  RESERVED_GEOMETRY: 26,
  PASSWORDS: 68,
  RESERVED_TAIL: 88,
};
export const IMAGE_FLAG = {
  PRIVACY: 1,
  DSFID_LOCKED: 2,
  AFI_LOCKED: 4,
  EAS_LOCKED: 8,
  RESERVED: 0xf0,
};
