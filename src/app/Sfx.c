#pragma once

#include "../unity.h"  // IWYU pragma: keep

// The shutter sound, mixed into sokol_audio's stream callback.
//
// Synthesized rather than shipped as a binary asset: a camera shutter is two
// short filtered-noise transients (mirror up, mirror down) over a low thump,
// which is a dozen lines of DSP and keeps the repository free of a vendored
// audio file and a decoder for it. $CAMERA_SFX overrides it with a 16-bit PCM
// WAV for anyone who wants a different click.
//
// The one-shot is driven by an int cursor the audio thread owns: the GUI thread
// only ever moves it back to 0, which is a benign race -- the worst case is a
// click that restarts a few milliseconds early.

#define SFX_SAMPLE_RATE (44100)
#define SFX_MAX_SAMPLES (SFX_SAMPLE_RATE / 2)  // half a second is plenty

// @class Sfx
// Function | Purpose
// --- | ---
// Sfx__synth(s) | Fill the buffer with a synthesized shutter click
// Sfx__loadWav(s, path) | Replace it with a 16-bit PCM WAV's first channel
// Sfx__init(s) | Load $CAMERA_SFX if set, else synthesize
// Sfx__trigger(s) | Start the one-shot from the beginning
// Sfx__mix(s, samples, frames, channels) | Mix the one-shot into an audio buffer
// Sfx__writeWav(s, path) | Dump the current sound as a mono 16-bit WAV

typedef struct {
  f32 pcm[SFX_MAX_SAMPLES];
  u32 ct;  // samples actually present
  volatile u32 cursor;  // SFX_MAX_SAMPLES or beyond means idle
} Sfx;

// a tiny deterministic PRNG so the click is identical on every run
static u32 _Sfx__rand(u32* state) {
  *state = (*state * 1664525u) + 1013904223u;
  return *state;
}

// noise in [-1, 1)
static f32 _Sfx__noise(u32* state) {
  return (f32)(_Sfx__rand(state) >> 8) / 8388608.0f - 1.0f;
}

// Fill the buffer with a synthesized shutter click
void Sfx__synth(Sfx* s) {
  memset(s, 0, sizeof(*s));
  s->ct = SFX_SAMPLE_RATE / 5;  // 200 ms
  s->cursor = SFX_MAX_SAMPLES;  // idle

  u32 seed = 0x5EED1234u;
  // one-pole low-pass state, which gives the noise a body rather than a hiss
  f32 lp = 0.0f;
  const u32 click2 = (u32)(0.085f * (f32)SFX_SAMPLE_RATE);  // second transient

  for (u32 i = 0; i < s->ct && i < SFX_MAX_SAMPLES; i++) {
    f32 t = (f32)i / (f32)SFX_SAMPLE_RATE;

    // two transients, each an exponentially decaying noise burst
    f32 env1 = expf(-t * 90.0f);
    f32 env2 = i >= click2 ? expf(-((f32)(i - click2) / (f32)SFX_SAMPLE_RATE) * 70.0f) : 0.0f;
    f32 n = _Sfx__noise(&seed);
    lp += (n - lp) * 0.45f;  // ~6 kHz corner
    f32 clicks = lp * (env1 * 0.85f + env2 * 0.65f);

    // a low thump under the first transient, for weight
    f32 thump = sinf(t * 2.0f * 3.14159265f * 140.0f) * expf(-t * 55.0f) * 0.22f;

    f32 v = clicks + thump;
    if (v > 1.0f) {
      v = 1.0f;
    }
    if (v < -1.0f) {
      v = -1.0f;
    }
    s->pcm[i] = v * 0.7f;  // headroom; this plays over whatever else is running
  }
}

// read a little-endian u32 from a byte buffer
static u32 _Sfx__le32(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// read a little-endian u16 from a byte buffer
static u16 _Sfx__le16(const u8* p) {
  return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

// Replace the sound with a 16-bit PCM WAV's first channel
bool Sfx__loadWav(Sfx* s, const char* path) {
  Arena* a = Arena__alloc(4ULL * 1024ULL * 1024ULL);
  if (NULL == a) {
    return false;
  }
  u8* buf = NULL;
  u64 len = 0;
  bool ok = false;

  if (!File__readAll(path, a, &buf, &len) || len < 44) {
    LOG_ERRORF("sfx: cannot read %s", path);
    Arena__free(a);
    return false;
  }
  if (!cstr__eqn(4, (const char*)buf, "RIFF") || !cstr__eqn(4, (const char*)buf + 8, "WAVE")) {
    LOG_ERRORF("sfx: %s is not a RIFF/WAVE file", path);
    Arena__free(a);
    return false;
  }

  // walk the chunk list looking for "fmt " then "data"
  u16 channels = 1, bits = 16, format = 1;
  u32 rate = SFX_SAMPLE_RATE;
  u64 at = 12;
  for (u32 i = 0; i < MAX_LOOP_ITER && at + 8 <= len; i++) {
    const u8* id = buf + at;
    u32 sz = _Sfx__le32(buf + at + 4);
    u64 body = at + 8;
    if (body + sz > len) {
      sz = (u32)(len - body);  // tolerate a truncated final chunk
    }
    if (cstr__eqn(4, (const char*)id, "fmt ") && sz >= 16) {
      format = _Sfx__le16(buf + body);
      channels = _Sfx__le16(buf + body + 2);
      rate = _Sfx__le32(buf + body + 4);
      bits = _Sfx__le16(buf + body + 14);
    } else if (cstr__eqn(4, (const char*)id, "data")) {
      if (1 != format || 16 != bits || 0 == channels) {
        LOG_ERRORF(
            "sfx: %s must be 16-bit PCM (got format %lu, %lu bits)",
            path,
            (unsigned long)format,
            (unsigned long)bits);
        break;
      }
      if (rate != SFX_SAMPLE_RATE) {
        // no resampler here; the click just plays at a different pitch
        LOG_INFOF(
            "sfx: %s is %lu Hz, playing at %d Hz",
            path,
            (unsigned long)rate,
            SFX_SAMPLE_RATE);
      }
      u32 frames = sz / (u32)(2u * channels);
      if (frames > SFX_MAX_SAMPLES) {
        frames = SFX_MAX_SAMPLES;
      }
      memset(s, 0, sizeof(*s));
      for (u32 f = 0; f < frames; f++) {
        s16 v = (s16)_Sfx__le16(buf + body + (u64)f * 2ULL * (u64)channels);
        s->pcm[f] = (f32)v / 32768.0f;  // first channel only
      }
      s->ct = frames;
      s->cursor = SFX_MAX_SAMPLES;
      ok = frames > 0;
      break;
    }
    at = body + sz + (sz & 1u);  // chunks are word-aligned
  }

  Arena__free(a);
  if (ok) {
    LOG_INFOF("sfx: loaded %lu samples from %s", (unsigned long)s->ct, path);
  }
  return ok;
}

// Load $CAMERA_SFX if set, else synthesize
void Sfx__init(Sfx* s) {
  const char* path = getenv("CAMERA_SFX");
  if (NULL != path && 0 != path[0] && Sfx__loadWav(s, path)) {
    return;
  }
  if (NULL != path && 0 != path[0]) {
    LOG_ERRORF("sfx: falling back to the built-in click");
  }
  Sfx__synth(s);
}

// Start the one-shot from the beginning
static inline void Sfx__trigger(Sfx* s) {
  s->cursor = 0;
}

// Mix the one-shot into an interleaved float audio buffer
void Sfx__mix(Sfx* s, f32* samples, u32 frames, u32 channels) {
  u32 at = s->cursor;
  for (u32 i = 0; i < frames && i < MAX_LOOP_ITER; i++) {
    f32 v = at < s->ct ? s->pcm[at] : 0.0f;
    for (u32 ch = 0; ch < channels; ch++) {
      samples[i * channels + ch] += v;
    }
    if (at < SFX_MAX_SAMPLES) {
      at++;
    }
  }
  s->cursor = at;
}

// Dump the current sound as a mono 16-bit WAV (so it can be edited and reused)
bool Sfx__writeWav(const Sfx* s, const char* path) {
  u32 data_sz = s->ct * 2u;
  u32 total = 44u + data_sz;
  u8* buf = (u8*)malloc(total);
  if (NULL == buf) {
    return false;
  }

  u32 at = 0;
  // RIFF header
  memcpy(buf + at, "RIFF", 4);
  at += 4;
  u32 riff_sz = total - 8u;
  buf[at++] = (u8)(riff_sz & 0xFF);
  buf[at++] = (u8)((riff_sz >> 8) & 0xFF);
  buf[at++] = (u8)((riff_sz >> 16) & 0xFF);
  buf[at++] = (u8)((riff_sz >> 24) & 0xFF);
  memcpy(buf + at, "WAVEfmt ", 8);
  at += 8;
  // fmt chunk: 16 bytes, PCM, mono, 16-bit
  static const u8 FMT[4] = {16, 0, 0, 0};
  memcpy(buf + at, FMT, 4);
  at += 4;
  buf[at++] = 1;  // format: PCM
  buf[at++] = 0;
  buf[at++] = 1;  // channels: mono
  buf[at++] = 0;
  u32 rate = SFX_SAMPLE_RATE;
  u32 byte_rate = rate * 2u;
  for (u32 i = 0; i < 4; i++) {
    buf[at++] = (u8)((rate >> (8u * i)) & 0xFF);
  }
  for (u32 i = 0; i < 4; i++) {
    buf[at++] = (u8)((byte_rate >> (8u * i)) & 0xFF);
  }
  buf[at++] = 2;  // block align
  buf[at++] = 0;
  buf[at++] = 16;  // bits per sample
  buf[at++] = 0;
  // data chunk
  memcpy(buf + at, "data", 4);
  at += 4;
  for (u32 i = 0; i < 4; i++) {
    buf[at++] = (u8)((data_sz >> (8u * i)) & 0xFF);
  }
  for (u32 i = 0; i < s->ct; i++) {
    f32 v = s->pcm[i];
    if (v > 1.0f) {
      v = 1.0f;
    }
    if (v < -1.0f) {
      v = -1.0f;
    }
    // round rather than truncate; truncation biases every sample toward zero
    // and makes a load/save round trip lose a least-significant bit
    s16 q = (s16)(v * 32767.0f + (v < 0.0f ? -0.5f : 0.5f));
    buf[at++] = (u8)((u16)q & 0xFF);
    buf[at++] = (u8)(((u16)q >> 8) & 0xFF);
  }

  bool ok = File__mkdirpFor(path) && File__writeAll(path, buf, at);
  free(buf);
  if (ok) {
    LOG_INFOF("wrote %lu samples to %s", (unsigned long)s->ct, path);
  }
  return ok;
}
