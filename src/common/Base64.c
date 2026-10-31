#pragma once

#include "../unity.h"  // IWYU pragma: keep

// @class Base64
// Function | Purpose
// --- | ---
// Base64__encodedLen(len) | Bytes needed to hold the encoding of len bytes (incl. NUL)
// Base64__encode(dst, dst_sz, src, len) | Encode src into dst as standard base64

// character set
static const char _BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Bytes needed to hold the encoding of len bytes, including the NUL terminator
static inline u64 Base64__encodedLen(u64 len) {
  return ((len + 2ULL) / 3ULL) * 4ULL + 1ULL;
}

// Encode src into dst as standard base64; false when dst is too small
bool Base64__encode(char* dst, u64 dst_sz, const u8* src, u64 len) {
  if (NULL == dst || NULL == src) {
    return false;
  }
  if (dst_sz < Base64__encodedLen(len)) {
    return false;
  }

  u64 i = 0, j = 0;
  for (; i + 2 < len; i += 3) {
    u32 triple = ((u32)src[i] << 16) | ((u32)src[i + 1] << 8) | (u32)src[i + 2];
    dst[j++] = _BASE64_CHARS[(triple >> 18) & 0x3F];
    dst[j++] = _BASE64_CHARS[(triple >> 12) & 0x3F];
    dst[j++] = _BASE64_CHARS[(triple >> 6) & 0x3F];
    dst[j++] = _BASE64_CHARS[triple & 0x3F];
  }

  // tail: 1 or 2 remaining bytes get '=' padding
  u64 rem = len - i;
  if (1 == rem) {
    u32 triple = (u32)src[i] << 16;
    dst[j++] = _BASE64_CHARS[(triple >> 18) & 0x3F];
    dst[j++] = _BASE64_CHARS[(triple >> 12) & 0x3F];
    dst[j++] = '=';
    dst[j++] = '=';
  } else if (2 == rem) {
    u32 triple = ((u32)src[i] << 16) | ((u32)src[i + 1] << 8);
    dst[j++] = _BASE64_CHARS[(triple >> 18) & 0x3F];
    dst[j++] = _BASE64_CHARS[(triple >> 12) & 0x3F];
    dst[j++] = _BASE64_CHARS[(triple >> 6) & 0x3F];
    dst[j++] = '=';
  }

  dst[j] = 0;
  return true;
}
