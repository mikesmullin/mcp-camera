#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// @describe Jpeg
// @tag common
//
// The encoder has no reference implementation to diff against in-process, so
// these scenarios check the invariants a decoder relies on (markers, segment
// structure, byte stuffing) and leave pixel fidelity to the round-trip check in
// test/roundtrip.sh, which decodes the output with an independent decoder.

// paint a YUYV422 frame of vertical colour bars with a grey ramp underneath
static void paintBars(u8* yuyv, u32 w, u32 h) {
  // BT.601 limited-range YCbCr for the standard bars
  static const u8 BARS[8][3] = {
      {235, 128, 128},  // white
      {210, 16, 146},  // yellow
      {170, 166, 16},  // cyan
      {145, 54, 34},  // green
      {106, 202, 222},  // magenta
      {81, 90, 240},  // red
      {41, 240, 110},  // blue
      {16, 128, 128},  // black
  };
  for (u32 y = 0; y < h; y++) {
    u8* row = yuyv + (u64)y * (u64)w * 2ULL;
    for (u32 x = 0; x + 1 < w; x += 2) {
      u32 bar = (x * 8u) / w;
      if (bar > 7) {
        bar = 7;
      }
      u8 luma = BARS[bar][0];
      if (y > h / 2) {
        // bottom half: a horizontal luma ramp, to exercise the DC predictor
        luma = (u8)(16u + (x * 219u) / (w > 1 ? w - 1 : 1));
      }
      row[x * 2 + 0] = luma;
      row[x * 2 + 1] = BARS[bar][1];
      row[x * 2 + 2] = luma;
      row[x * 2 + 3] = BARS[bar][2];
    }
  }
}

int main(int argc, char* argv[]) {
  Console__init();
  Jpeg__init();

  const u32 W = 160, H = 120;
  Arena* arena = Arena__allocZ(8ULL * 1024ULL * 1024ULL);
  ASSERT(NULL != arena);

  u8* yuyv = (u8*)Arena__push(arena, (u64)W * H * 2ULL);
  paintBars(yuyv, W, H);

  u64 cap = Jpeg__boundLen(W, H);
  u8* out = (u8*)Arena__push(arena, cap);

  // ---
  // Scenario: a frame encodes to a structurally valid JPEG
  u64 len = 0;
  {
    len = Jpeg__encodeYuyv(out, cap, yuyv, W, H, 90);
    ASSERT_CONTEXT(len > 0, "encode returned 0 bytes");
    ASSERT_CONTEXT(
        len < cap,
        "encode overran its bound: %llu >= %llu",
        (unsigned long long)len,
        (unsigned long long)cap);

    // SOI and EOI
    ASSERT_CONTEXT(0xFF == out[0] && 0xD8 == out[1], "missing SOI");
    ASSERT_CONTEXT(0xFF == out[len - 2] && 0xD9 == out[len - 1], "missing EOI");

    // the header segments a baseline decoder needs, in order
    bool saw_app0 = false, saw_dqt = false, saw_sof0 = false, saw_sos = false;
    u32 dht_ct = 0;
    u64 at = 2;
    for (u32 i = 0; i < 64 && at + 3 < len; i++) {
      ASSERT_CONTEXT(0xFF == out[at], "expected a marker at offset %llu", (unsigned long long)at);
      u8 marker = out[at + 1];
      u32 seg = ((u32)out[at + 2] << 8) | (u32)out[at + 3];
      if (0xE0 == marker) {
        saw_app0 = true;
      } else if (0xDB == marker) {
        saw_dqt = true;
      } else if (0xC0 == marker) {
        saw_sof0 = true;
        // SOF0 payload: precision, height, width, component count
        ASSERT_CONTEXT(8 == out[at + 4], "sample precision must be 8");
        u32 hh = ((u32)out[at + 5] << 8) | (u32)out[at + 6];
        u32 ww = ((u32)out[at + 7] << 8) | (u32)out[at + 8];
        ASSERT_CONTEXT(
            H == hh && W == ww,
            "SOF0 says %lux%lu, expected %lux%lu",
            (unsigned long)ww,
            (unsigned long)hh,
            (unsigned long)W,
            (unsigned long)H);
        ASSERT_CONTEXT(3 == out[at + 9], "expected 3 components");
      } else if (0xC4 == marker) {
        dht_ct++;
      } else if (0xDA == marker) {
        saw_sos = true;
        break;  // entropy-coded data follows; marker walking ends here
      }
      at += 2 + seg;
    }
    ASSERT_CONTEXT(saw_app0, "missing APP0/JFIF");
    ASSERT_CONTEXT(saw_dqt, "missing DQT");
    ASSERT_CONTEXT(saw_sof0, "missing SOF0");
    ASSERT_CONTEXT(4 == dht_ct, "expected 4 DHT segments, saw %lu", (unsigned long)dht_ct);
    ASSERT_CONTEXT(saw_sos, "missing SOS");
  }

  // ---
  // Scenario: scan data is byte-stuffed, so no stray marker can appear
  {
    // find SOS, then verify every 0xFF in the scan is followed by 0x00
    u64 sos = 0;
    for (u64 i = 2; i + 1 < len; i++) {
      if (0xFF == out[i] && 0xDA == out[i + 1]) {
        sos = i;
        break;
      }
    }
    ASSERT_CONTEXT(sos > 0, "could not locate SOS");
    u64 scan = sos + 2 + (((u64)out[sos + 2] << 8) | (u64)out[sos + 3]);
    u32 stuffed = 0;
    for (u64 i = scan; i + 1 < len - 2; i++) {
      if (0xFF != out[i]) {
        continue;
      }
      ASSERT_CONTEXT(
          0x00 == out[i + 1],
          "unstuffed 0xFF%02X inside the scan at %llu",
          out[i + 1],
          (unsigned long long)i);
      stuffed++;
      i++;  // skip the stuffed zero
    }
    LOG_INFOF("scan contained %lu stuffed bytes", (unsigned long)stuffed);
  }

  // ---
  // Scenario: quality changes the size monotonically
  {
    u64 lo = Jpeg__encodeYuyv(out, cap, yuyv, W, H, 20);
    u64 mid = Jpeg__encodeYuyv(out, cap, yuyv, W, H, 60);
    u64 hi = Jpeg__encodeYuyv(out, cap, yuyv, W, H, 95);
    ASSERT_CONTEXT(lo > 0 && mid > 0 && hi > 0, "an encode failed");
    ASSERT_CONTEXT(
        lo < mid && mid < hi,
        "size should grow with quality: %llu %llu %llu",
        (unsigned long long)lo,
        (unsigned long long)mid,
        (unsigned long long)hi);
  }

  // ---
  // Scenario: odd dimensions are padded, not rejected
  {
    const u32 OW = 37, OH = 23;
    u8* odd = (u8*)Arena__push(arena, (u64)OW * OH * 2ULL);
    paintBars(odd, OW, OH);
    u64 n = Jpeg__encodeYuyv(out, cap, odd, OW, OH, 85);
    ASSERT_CONTEXT(n > 0, "odd dimensions failed to encode");
    ASSERT_CONTEXT(0xFF == out[0] && 0xD8 == out[1], "odd-size output missing SOI");
  }

  // ---
  // Scenario: an undersized buffer fails rather than overflowing
  {
    u64 n = Jpeg__encodeYuyv(out, 512, yuyv, W, H, 95);
    ASSERT_CONTEXT(0 == n, "a 512 byte buffer should have been refused");
  }

  // ---
  // Scenario: bad arguments are refused
  {
    ASSERT(0 == Jpeg__encodeYuyv(NULL, cap, yuyv, W, H, 90));
    ASSERT(0 == Jpeg__encodeYuyv(out, cap, NULL, W, H, 90));
    ASSERT(0 == Jpeg__encodeYuyv(out, cap, yuyv, 0, H, 90));
    ASSERT(0 == Jpeg__encodeYuyv(out, cap, yuyv, W, 0, 90));
  }

  // optionally dump a frame so an external decoder can verify the pixels
  if (argc > 1) {
    u64 n = Jpeg__encodeYuyv(out, cap, yuyv, W, H, 90);
    ASSERT(n > 0);
    ASSERT(File__writeAll(argv[1], out, n));
    Console__out("wrote %llu bytes to %s\n", (unsigned long long)n, argv[1]);
  }

  Arena__free(arena);
  Console__out("Jpeg: all scenarios passed\n");
  return 0;
}
