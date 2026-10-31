#pragma once

#include <math.h>  // IWYU pragma: keep // cosf(), sqrtf()

#include "../unity.h"  // IWYU pragma: keep

// Minimal baseline (sequential DCT, Huffman) JPEG encoder.
//
// Deliberately 4:4:4 (all sampling factors 1x1) so one MCU is exactly one 8x8
// block per component. That keeps the interleave trivial and the output
// obviously correct, at the cost of ~1.4x the bytes of a 4:2:0 encoder --
// irrelevant for a 640x480 webcam frame, and it keeps small on-screen text
// legible for the OCR backend.
//
// Tables are the Annex K examples from ITU-T T.81, which every decoder accepts.

// @class Jpeg
// Function | Purpose
// --- | ---
// Jpeg__init() | Build the shared cosine table (call once, before any encode)
// Jpeg__boundLen(w, h) | Worst-case output size for a w*h frame
// Jpeg__encodeYuyv(dst, dst_cap, yuyv, w, h, quality) | Encode a YUYV422 frame, return bytes

// zigzag slot -> natural (row-major) index
static const u8 _JPEG_ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  //
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,  //
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,  //
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,  //
};

// Annex K.1 luminance quantization table, natural order
static const u8 _JPEG_QY[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  //
    12, 12, 14, 19, 26,  58,  60,  55,  //
    14, 13, 16, 24, 40,  57,  69,  56,  //
    14, 17, 22, 29, 51,  87,  80,  62,  //
    18, 22, 37, 56, 68,  109, 103, 77,  //
    24, 35, 55, 64, 81,  104, 113, 92,  //
    49, 64, 78, 87, 103, 121, 120, 101,  //
    72, 92, 95, 98, 112, 100, 103, 99,  //
};

// Annex K.2 chrominance quantization table, natural order
static const u8 _JPEG_QC[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,  //
    18, 21, 26, 66, 99, 99, 99, 99,  //
    24, 26, 56, 99, 99, 99, 99, 99,  //
    47, 66, 99, 99, 99, 99, 99, 99,  //
    99, 99, 99, 99, 99, 99, 99, 99,  //
    99, 99, 99, 99, 99, 99, 99, 99,  //
    99, 99, 99, 99, 99, 99, 99, 99,  //
    99, 99, 99, 99, 99, 99, 99, 99,  //
};

// Annex K.3 Huffman specifications: counts[i] = number of codes of length i+1
static const u8 _JPEG_DC_LUMA_BITS[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const u8 _JPEG_DC_LUMA_VALS[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const u8 _JPEG_DC_CHROMA_BITS[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const u8 _JPEG_DC_CHROMA_VALS[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const u8 _JPEG_AC_LUMA_BITS[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const u8 _JPEG_AC_LUMA_VALS[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static const u8 _JPEG_AC_CHROMA_BITS[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const u8 _JPEG_AC_CHROMA_VALS[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

// a canonical Huffman code table, indexed by symbol value
typedef struct {
  u16 code[256];
  u8 len[256];  // 0 = symbol unused
} JpegHuff;

// the encoder's whole working state (no globals: unit tests run it standalone)
typedef struct {
  u8* dst;
  u64 cap;
  u64 len;
  bool overflow;  // sticky

  u32 bit_buf;  // pending bits, left-aligned
  u32 bit_ct;  // how many bits are pending

  u8 qy[64], qc[64];  // natural order, quality-scaled
  JpegHuff dc_luma, ac_luma, dc_chroma, ac_chroma;
} JpegEnc;

// Worst-case output size for a w*h frame
static inline u64 Jpeg__boundLen(u32 w, u32 h) {
  // 4:4:4 at worst spends well under 3 bytes/pixel plus headers
  return (u64)w * (u64)h * 3ULL + 65536ULL;
}

// append one byte, marking overflow rather than writing past the end
static void _Jpeg__u8(JpegEnc* e, u8 b) {
  if (e->overflow) {
    return;
  }
  if (e->len >= e->cap) {
    e->overflow = true;
    return;
  }
  e->dst[e->len++] = b;
}

static void _Jpeg__u16be(JpegEnc* e, u16 v) {
  _Jpeg__u8(e, (u8)(v >> 8));
  _Jpeg__u8(e, (u8)(v & 0xFF));
}

static void _Jpeg__bytes(JpegEnc* e, const u8* src, u32 n) {
  for (u32 i = 0; i < n && i < MAX_LOOP_ITER; i++) {
    _Jpeg__u8(e, src[i]);
  }
}

// write `n` low bits of `v`, MSB first, stuffing 0x00 after any emitted 0xFF
static void _Jpeg__putBits(JpegEnc* e, u32 v, u32 n) {
  if (0 == n) {
    return;
  }
  e->bit_buf = (e->bit_buf << n) | (v & ((1u << n) - 1u));
  e->bit_ct += n;
  while (e->bit_ct >= 8) {
    u8 byte = (u8)((e->bit_buf >> (e->bit_ct - 8)) & 0xFFu);
    e->bit_ct -= 8;
    _Jpeg__u8(e, byte);
    if (0xFF == byte) {
      _Jpeg__u8(e, 0x00);  // byte stuffing (T.81 B.1.1.5)
    }
  }
}

// pad the final partial byte with 1-bits, per T.81
static void _Jpeg__flushBits(JpegEnc* e) {
  if (e->bit_ct > 0) {
    u32 pad = 8 - e->bit_ct;
    _Jpeg__putBits(e, (1u << pad) - 1u, pad);
  }
  e->bit_buf = 0;
  e->bit_ct = 0;
}

// expand a (counts, values) Huffman spec into a symbol-indexed code table
static void _Jpeg__buildHuff(JpegHuff* h, const u8* counts, const u8* vals, u32 nvals) {
  memset(h, 0, sizeof(*h));
  u16 code = 0;
  u32 k = 0;
  for (u32 bits = 1; bits <= 16; bits++) {
    for (u32 i = 0; i < counts[bits - 1] && k < nvals; i++) {
      h->code[vals[k]] = code;
      h->len[vals[k]] = (u8)bits;
      code++;
      k++;
    }
    code = (u16)(code << 1);
  }
}

// scale a base quantization table for `quality` (libjpeg's mapping)
static void _Jpeg__scaleQuant(u8* dst, const u8* base, u32 quality) {
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  u32 scale = quality < 50 ? 5000u / quality : 200u - quality * 2u;
  for (u32 i = 0; i < 64; i++) {
    u32 v = ((u32)base[i] * scale + 50u) / 100u;
    if (v < 1) {
      v = 1;
    }
    if (v > 255) {
      v = 255;
    }
    dst[i] = (u8)v;
  }
}

// ct[k][n] = c(k) * cos((2n+1) k pi / 16) * 0.5, with c(0) = 1/sqrt(2).
// File-scope and built once by Jpeg__init() so encoding needs no lazy init
// (and stays safe to call from the preview's IPC thread).
static f32 _JPEG_CT[8][8];
static bool _JPEG_CT_READY = false;

// Build the shared cosine table (call once, before any encode)
void Jpeg__init(void) {
  for (u32 k = 0; k < 8; k++) {
    f32 c = 0 == k ? 0.70710678f : 1.0f;
    for (u32 n = 0; n < 8; n++) {
      _JPEG_CT[k][n] = 0.5f * c * cosf((f32)((2 * n + 1) * k) * 3.14159265358979f / 16.0f);
    }
  }
  _JPEG_CT_READY = true;
}

// 8x8 forward DCT-II, separable, orthonormal JPEG scaling (T.81 A.3.3)
static void _Jpeg__fdct(const f32* in, f32* out) {
  const f32(*ct)[8] = _JPEG_CT;
  f32 tmp[64];
  // pass 1: transform along x, producing horizontal frequency u
  for (u32 y = 0; y < 8; y++) {
    for (u32 u = 0; u < 8; u++) {
      f32 s = 0.0f;
      for (u32 x = 0; x < 8; x++) {
        s += ct[u][x] * in[y * 8 + x];
      }
      tmp[y * 8 + u] = s;
    }
  }
  // pass 2: transform along y, producing vertical frequency v
  for (u32 u = 0; u < 8; u++) {
    for (u32 v = 0; v < 8; v++) {
      f32 s = 0.0f;
      for (u32 y = 0; y < 8; y++) {
        s += ct[v][y] * tmp[y * 8 + u];
      }
      out[v * 8 + u] = s;
    }
  }
}

// how many bits are needed to hold |v| (the JPEG "category" of a coefficient)
static u32 _Jpeg__category(s32 v) {
  u32 a = (u32)(v < 0 ? -v : v);
  u32 n = 0;
  while (a > 0 && n < 16) {
    a >>= 1;
    n++;
  }
  return n;
}

// emit one Huffman symbol
static void _Jpeg__putSym(JpegEnc* e, const JpegHuff* h, u8 sym) {
  if (0 == h->len[sym]) {
    e->overflow = true;  // table/symbol mismatch is a programming error
    return;
  }
  _Jpeg__putBits(e, h->code[sym], h->len[sym]);
}

// Huffman-encode one quantized 8x8 block; returns the DC value for prediction
static s32 _Jpeg__encodeBlock(
    JpegEnc* e,
    const f32* block,  // 64 centred samples, natural order
    const u8* quant,  // 64 quantizers, natural order
    const JpegHuff* dc,
    const JpegHuff* ac,
    s32 prev_dc) {
  f32 freq[64];
  _Jpeg__fdct(block, freq);

  // quantize into zigzag order
  s32 zz[64];
  for (u32 i = 0; i < 64; i++) {
    u32 nat = _JPEG_ZIGZAG[i];
    f32 q = freq[nat] / (f32)quant[nat];
    zz[i] = (s32)(q < 0.0f ? q - 0.5f : q + 0.5f);
  }

  // DC: difference from the previous block of this component
  s32 diff = zz[0] - prev_dc;
  u32 cat = _Jpeg__category(diff);
  _Jpeg__putSym(e, dc, (u8)cat);
  if (cat > 0) {
    s32 v = diff < 0 ? diff - 1 : diff;  // negatives use the one's-complement form
    _Jpeg__putBits(e, (u32)v, cat);
  }

  // AC: run-length of zeros, then the coefficient category
  u32 run = 0;
  u32 last = 0;  // index of the final nonzero coefficient
  for (u32 i = 1; i < 64; i++) {
    if (0 != zz[i]) {
      last = i;
    }
  }
  for (u32 i = 1; i <= last; i++) {
    if (0 == zz[i]) {
      run++;
      continue;
    }
    while (run >= 16) {
      _Jpeg__putSym(e, ac, 0xF0);  // ZRL: sixteen zeros
      run -= 16;
    }
    u32 s = _Jpeg__category(zz[i]);
    _Jpeg__putSym(e, ac, (u8)((run << 4) | s));
    s32 v = zz[i] < 0 ? zz[i] - 1 : zz[i];
    _Jpeg__putBits(e, (u32)v, s);
    run = 0;
  }
  if (last < 63) {
    _Jpeg__putSym(e, ac, 0x00);  // EOB
  }

  return zz[0];
}

// write every header segment up to and including SOS
static void _Jpeg__writeHeaders(JpegEnc* e, u32 w, u32 h) {
  _Jpeg__u16be(e, 0xFFD8);  // SOI

  // APP0 / JFIF
  _Jpeg__u16be(e, 0xFFE0);
  _Jpeg__u16be(e, 16);
  _Jpeg__bytes(e, (const u8*)"JFIF\0", 5);
  _Jpeg__u16be(e, 0x0102);  // version 1.2
  _Jpeg__u8(e, 0);  // density units: none
  _Jpeg__u16be(e, 1);  // x density
  _Jpeg__u16be(e, 1);  // y density
  _Jpeg__u8(e, 0);  // no thumbnail
  _Jpeg__u8(e, 0);

  // DQT: both tables in one segment, zigzag order
  _Jpeg__u16be(e, 0xFFDB);
  _Jpeg__u16be(e, 2 + 2 * (1 + 64));
  _Jpeg__u8(e, 0x00);  // 8-bit precision, table 0
  for (u32 i = 0; i < 64; i++) {
    _Jpeg__u8(e, e->qy[_JPEG_ZIGZAG[i]]);
  }
  _Jpeg__u8(e, 0x01);  // 8-bit precision, table 1
  for (u32 i = 0; i < 64; i++) {
    _Jpeg__u8(e, e->qc[_JPEG_ZIGZAG[i]]);
  }

  // SOF0: baseline, 3 components, all sampling 1x1
  _Jpeg__u16be(e, 0xFFC0);
  _Jpeg__u16be(e, 8 + 3 * 3);
  _Jpeg__u8(e, 8);  // sample precision
  _Jpeg__u16be(e, (u16)h);
  _Jpeg__u16be(e, (u16)w);
  _Jpeg__u8(e, 3);  // component count
  _Jpeg__u8(e, 1);  // Y
  _Jpeg__u8(e, 0x11);  // H=1, V=1
  _Jpeg__u8(e, 0);  // quant table 0
  _Jpeg__u8(e, 2);  // Cb
  _Jpeg__u8(e, 0x11);
  _Jpeg__u8(e, 1);  // quant table 1
  _Jpeg__u8(e, 3);  // Cr
  _Jpeg__u8(e, 0x11);
  _Jpeg__u8(e, 1);

  // DHT: four tables, each its own segment
  _Jpeg__u16be(e, 0xFFC4);
  _Jpeg__u16be(e, (u16)(3 + 16 + 12));
  _Jpeg__u8(e, 0x00);  // class 0 (DC), table 0
  _Jpeg__bytes(e, _JPEG_DC_LUMA_BITS, 16);
  _Jpeg__bytes(e, _JPEG_DC_LUMA_VALS, 12);

  _Jpeg__u16be(e, 0xFFC4);
  _Jpeg__u16be(e, (u16)(3 + 16 + 162));
  _Jpeg__u8(e, 0x10);  // class 1 (AC), table 0
  _Jpeg__bytes(e, _JPEG_AC_LUMA_BITS, 16);
  _Jpeg__bytes(e, _JPEG_AC_LUMA_VALS, 162);

  _Jpeg__u16be(e, 0xFFC4);
  _Jpeg__u16be(e, (u16)(3 + 16 + 12));
  _Jpeg__u8(e, 0x01);  // class 0 (DC), table 1
  _Jpeg__bytes(e, _JPEG_DC_CHROMA_BITS, 16);
  _Jpeg__bytes(e, _JPEG_DC_CHROMA_VALS, 12);

  _Jpeg__u16be(e, 0xFFC4);
  _Jpeg__u16be(e, (u16)(3 + 16 + 162));
  _Jpeg__u8(e, 0x11);  // class 1 (AC), table 1
  _Jpeg__bytes(e, _JPEG_AC_CHROMA_BITS, 16);
  _Jpeg__bytes(e, _JPEG_AC_CHROMA_VALS, 162);

  // SOS
  _Jpeg__u16be(e, 0xFFDA);
  _Jpeg__u16be(e, 6 + 2 * 3);
  _Jpeg__u8(e, 3);  // component count
  _Jpeg__u8(e, 1);  // Y
  _Jpeg__u8(e, 0x00);  // DC table 0, AC table 0
  _Jpeg__u8(e, 2);  // Cb
  _Jpeg__u8(e, 0x11);  // DC table 1, AC table 1
  _Jpeg__u8(e, 3);  // Cr
  _Jpeg__u8(e, 0x11);
  _Jpeg__u8(e, 0);  // Ss
  _Jpeg__u8(e, 63);  // Se
  _Jpeg__u8(e, 0);  // Ah/Al
}

// Encode a YUYV422 frame as a baseline JPEG into dst; returns bytes written (0 = failed)
u64 Jpeg__encodeYuyv(u8* dst, u64 dst_cap, const u8* yuyv, u32 w, u32 h, u32 quality) {
  if (NULL == dst || NULL == yuyv || 0 == w || 0 == h) {
    return 0;
  }
  if (w > MAX_FRAME_W || h > MAX_FRAME_H) {
    LOG_ERRORF("jpeg: frame %ux%u exceeds limits", w, h);
    return 0;
  }

  ASSERT_CONTEXT(_JPEG_CT_READY, "Jpeg__init() was never called");

  // ~3 KiB of tables; small enough to keep off the heap entirely
  JpegEnc enc;
  JpegEnc* e = &enc;
  memset(e, 0, sizeof(*e));
  e->dst = dst;
  e->cap = dst_cap;

  _Jpeg__scaleQuant(e->qy, _JPEG_QY, quality);
  _Jpeg__scaleQuant(e->qc, _JPEG_QC, quality);
  _Jpeg__buildHuff(&e->dc_luma, _JPEG_DC_LUMA_BITS, _JPEG_DC_LUMA_VALS, 12);
  _Jpeg__buildHuff(&e->ac_luma, _JPEG_AC_LUMA_BITS, _JPEG_AC_LUMA_VALS, 162);
  _Jpeg__buildHuff(&e->dc_chroma, _JPEG_DC_CHROMA_BITS, _JPEG_DC_CHROMA_VALS, 12);
  _Jpeg__buildHuff(&e->ac_chroma, _JPEG_AC_CHROMA_BITS, _JPEG_AC_CHROMA_VALS, 162);

  _Jpeg__writeHeaders(e, w, h);

  u32 mcu_x = (w + 7) / 8, mcu_y = (h + 7) / 8;
  s32 dc_y = 0, dc_cb = 0, dc_cr = 0;
  f32 by[64], bcb[64], bcr[64];

  for (u32 my = 0; my < mcu_y && !e->overflow; my++) {
    for (u32 mx = 0; mx < mcu_x && !e->overflow; mx++) {
      // gather one 8x8 block per component, replicating edge pixels when the
      // frame is not a multiple of 8
      for (u32 r = 0; r < 8; r++) {
        u32 sy = my * 8 + r;
        if (sy >= h) {
          sy = h - 1;
        }
        for (u32 c = 0; c < 8; c++) {
          u32 sx = mx * 8 + c;
          if (sx >= w) {
            sx = w - 1;
          }
          const u8* row = yuyv + (u64)sy * (u64)w * 2ULL;
          u8 y = row[sx * 2];
          // YUYV packs one Cb/Cr pair per two horizontal luma samples
          u32 pair = (sx & ~1u) * 2u;
          u8 cb = row[pair + 1];
          u8 cr = row[pair + 3];
          // V4L2 UVC delivers BT.601 limited range; JFIF wants full range
          f32 fy = ((f32)y - 16.0f) * (255.0f / 219.0f);
          f32 fcb = ((f32)cb - 128.0f) * (255.0f / 224.0f) + 128.0f;
          f32 fcr = ((f32)cr - 128.0f) * (255.0f / 224.0f) + 128.0f;
          if (fy < 0.0f) {
            fy = 0.0f;
          }
          if (fy > 255.0f) {
            fy = 255.0f;
          }
          if (fcb < 0.0f) {
            fcb = 0.0f;
          }
          if (fcb > 255.0f) {
            fcb = 255.0f;
          }
          if (fcr < 0.0f) {
            fcr = 0.0f;
          }
          if (fcr > 255.0f) {
            fcr = 255.0f;
          }
          by[r * 8 + c] = fy - 128.0f;  // centre on zero for the DCT
          bcb[r * 8 + c] = fcb - 128.0f;
          bcr[r * 8 + c] = fcr - 128.0f;
        }
      }

      dc_y = _Jpeg__encodeBlock(e, by, e->qy, &e->dc_luma, &e->ac_luma, dc_y);
      dc_cb = _Jpeg__encodeBlock(e, bcb, e->qc, &e->dc_chroma, &e->ac_chroma, dc_cb);
      dc_cr = _Jpeg__encodeBlock(e, bcr, e->qc, &e->dc_chroma, &e->ac_chroma, dc_cr);
    }
  }

  _Jpeg__flushBits(e);
  _Jpeg__u16be(e, 0xFFD9);  // EOI

  if (e->overflow) {
    LOG_ERRORF("jpeg: output buffer too small (%llu bytes)", (unsigned long long)dst_cap);
    return 0;
  }
  return e->len;
}
