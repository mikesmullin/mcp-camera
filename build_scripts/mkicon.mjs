#!/usr/bin/env node
// Draws the mcp-camera icon and emits it two ways:
//
//   assets/icon.h            RGBA8 arrays for sokol_app's window icon
//   assets/mcp-camera-N.png  PNGs for .desktop entries and launchers
//
// The icon is drawn rather than vendored, for the same reason the rest of this
// project is: one small script beats a binary blob nobody can edit. It is
// rendered with 4x supersampling because the 16x16 size has to stay readable.
//
//   node build_scripts/mkicon.mjs
//
// NOTE: a Linux ELF binary cannot carry an embedded icon. What a taskbar
// actually shows is the EWMH _NET_WM_ICON property, which sokol_app sets from
// the arrays in icon.h, plus the .desktop file's Icon= for launchers.

import {deflateSync} from 'node:zlib';
import {writeFileSync, mkdirSync} from 'node:fs';
import {join, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const ROOT = dirname(dirname(fileURLToPath(import.meta.url)));
const ASSETS = join(ROOT, 'assets');
const SIZES = [16, 32, 64, 128, 256];
const SS = 4;  // supersampling factor

// --- palette -------------------------------------------------------------
// Dark body so the silhouette reads on a light taskbar; a bright lens and the
// red shutter accent (the same red as the freeze border) carry it on a dark one.
const BODY_TOP = [0x3B, 0x42, 0x52, 255];
const BODY_BOT = [0x23, 0x28, 0x34, 255];
const HUMP = [0x4A, 0x53, 0x66, 255];
const RING = [0xD6, 0xDC, 0xE6, 255];
const GLASS_TOP = [0x5A, 0x92, 0xD8, 255];
const GLASS_BOT = [0x1C, 0x39, 0x66, 255];
const GLINT = [0xFF, 0xFF, 0xFF, 235];
const SHUTTER = [0xED, 0x1C, 0x24, 255];
const FLASH = [0xF2, 0xD9, 0x7A, 255];

// --- drawing primitives (all in 0..1 unit space) -------------------------

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const lerp = (a, b, t) => a + (b - a) * t;

// signed distance to a rounded rectangle; negative inside
function sdRoundRect(px, py, x0, y0, x1, y1, r) {
  const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  const hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
  const dx = Math.abs(px - cx) - hx, dy = Math.abs(py - cy) - hy;
  const ox = Math.max(dx, 0), oy = Math.max(dy, 0);
  return Math.hypot(ox, oy) + Math.min(Math.max(dx, dy), 0) - r;
}

const sdCircle = (px, py, cx, cy, r) => Math.hypot(px - cx, py - cy) - r;

// alpha coverage for a distance field, feathered over one output pixel
function cover(d, feather) {
  return clamp01(0.5 - d / feather);
}

// src-over composite, premultiplied internally
function over(dst, src, a) {
  if (a <= 0) return;
  const sa = (src[3] / 255) * a;
  if (sa <= 0) return;
  const da = dst[3] / 255;
  const oa = sa + da * (1 - sa);
  for (let i = 0; i < 3; i++) {
    dst[i] = oa <= 0 ? 0 : (src[i] * sa + dst[i] * da * (1 - sa)) / oa;
  }
  dst[3] = oa * 255;
}

// --- the icon itself ------------------------------------------------------

// Returns the RGBA colour at unit coords (x, y), y down.
// `feather` is one output pixel in unit space, so edges stay crisp at 16px and
// smooth at 256px.
function shade(x, y, feather) {
  const px = [0, 0, 0, 0];

  // viewfinder hump on top of the body
  const hump = sdRoundRect(x, y, 0.28, 0.145, 0.55, 0.30, 0.045);
  over(px, HUMP, cover(hump, feather));

  // camera body
  const body = sdRoundRect(x, y, 0.045, 0.255, 0.955, 0.875, 0.105);
  const bodyA = cover(body, feather);
  if (bodyA > 0) {
    // a soft top-to-bottom gradient gives the body some form
    const t = clamp01((y - 0.255) / (0.875 - 0.255));
    const col = [
      lerp(BODY_TOP[0], BODY_BOT[0], t),
      lerp(BODY_TOP[1], BODY_BOT[1], t),
      lerp(BODY_TOP[2], BODY_BOT[2], t),
      255,
    ];
    over(px, col, bodyA);
  }

  // flash window, upper left of the body
  over(px, FLASH, cover(sdRoundRect(x, y, 0.115, 0.335, 0.225, 0.40, 0.028), feather));

  // shutter button, upper right: the same red as the freeze border
  over(px, SHUTTER, cover(sdRoundRect(x, y, 0.745, 0.325, 0.895, 0.405, 0.035), feather));

  // lens: outer ring, then glass, then a glint
  const LCX = 0.5, LCY = 0.585;
  over(px, RING, cover(sdCircle(x, y, LCX, LCY, 0.232), feather));
  const glass = sdCircle(x, y, LCX, LCY, 0.176);
  const glassA = cover(glass, feather);
  if (glassA > 0) {
    const t = clamp01((y - (LCY - 0.176)) / 0.352);
    const col = [
      lerp(GLASS_TOP[0], GLASS_BOT[0], t),
      lerp(GLASS_TOP[1], GLASS_BOT[1], t),
      lerp(GLASS_TOP[2], GLASS_BOT[2], t),
      255,
    ];
    over(px, col, glassA);
  }
  over(px, GLINT, cover(sdCircle(x, y, LCX - 0.072, LCY - 0.080, 0.050), feather));

  return px;
}

// render one size with supersampling
function render(size) {
  const out = Buffer.alloc(size * size * 4);
  const n = size * SS;
  const feather = 1 / n;  // one supersample, so downsampling does the smoothing
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      let r = 0, g = 0, b = 0, a = 0;
      for (let sy = 0; sy < SS; sy++) {
        for (let sx = 0; sx < SS; sx++) {
          const ux = (x * SS + sx + 0.5) / n;
          const uy = (y * SS + sy + 0.5) / n;
          const p = shade(ux, uy, feather);
          const pa = p[3] / 255;
          r += p[0] * pa;
          g += p[1] * pa;
          b += p[2] * pa;
          a += pa;
        }
      }
      const ct = SS * SS;
      const i = (y * size + x) * 4;
      // un-premultiply so the stored pixels are straight RGBA
      out[i + 0] = a > 0 ? Math.round(r / a) : 0;
      out[i + 1] = a > 0 ? Math.round(g / a) : 0;
      out[i + 2] = a > 0 ? Math.round(b / a) : 0;
      out[i + 3] = Math.round((a / ct) * 255);
    }
  }
  return out;
}

// --- PNG writer (zlib is built in; no image dependency) -------------------

const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

function writePng(path, rgba, size) {
  // each scanline is prefixed with filter type 0 (None)
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0;
    rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 6;  // colour type: RGBA
  const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, {level: 9})),
    chunk('IEND', Buffer.alloc(0)),
  ]);
  writeFileSync(path, png);
  return png.length;
}

// --- emit -----------------------------------------------------------------

mkdirSync(ASSETS, {recursive: true});

const images = SIZES.map((s) => ({size: s, rgba: render(s)}));

for (const {size, rgba} of images) {
  const bytes = writePng(join(ASSETS, `mcp-camera-${size}.png`), rgba, size);
  console.log(`assets/mcp-camera-${size}.png  (${bytes} bytes)`);
}

// the C header carries only the sizes sokol hands to the window manager
const HEADER_SIZES = [16, 32, 64];
let h = `// GENERATED by build_scripts/mkicon.mjs -- do not edit by hand.
//
// The mcp-camera window icon, as straight (non-premultiplied) RGBA8, row-major
// from the top-left. Handed to sokol_app via sapp_desc.icon.images, which sets
// the EWMH _NET_WM_ICON property the window manager and taskbar read.
#pragma once

`;
for (const size of HEADER_SIZES) {
  const {rgba} = images.find((i) => i.size === size);
  h += `static const unsigned char ICON_${size}[${size} * ${size} * 4] = {\n`;
  for (let i = 0; i < rgba.length; i += 16) {
    h += '    ' + [...rgba.subarray(i, i + 16)].map((v) => String(v).padStart(3)).join(',') +
         ',\n';
  }
  h += '};\n\n';
}
writeFileSync(join(ASSETS, 'icon.h'), h);
console.log(`assets/icon.h  (${h.length} bytes, sizes ${HEADER_SIZES.join('/')})`);
