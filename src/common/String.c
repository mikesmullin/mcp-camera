#pragma once

#include "../unity.h"  // IWYU pragma: keep

// @class cstr
// Function | Purpose
// --- | ---
// cstr__len(c) | Length of a null-terminated string, bounded
// cstr__eq(a, b) | Compare two null-terminated strings for equality
// cstr__eqn(len, a, b) | Compare the first len bytes of two strings
// cstr__ieqn(len, a, b) | Case-insensitive compare of the first len bytes
// cstr__copy(dst, dst_sz, src) | Copy src into dst, always null-terminating
// cstr__copyn(dst, dst_sz, src, src_len) | Copy src_len bytes of src, always null-terminating
// cstr__cat(dst, dst_sz, src) | Append src to dst, always null-terminating
// cstr__endsWith(c, suffix) | Does c end with suffix?
// cstr__contains(hay, needle) | Does hay contain needle?
// cstr__icontains(hay, needle) | Case-insensitive substring test
// cstr__toU32(s, out) | Parse an unsigned decimal integer, strictly
// cstr__trimRight(c) | Strip trailing whitespace in place
// cstr__isLowerC(c) | Lowercase a single byte
// cstr__slugify(dst, dst_sz, src) | Reduce src to a safe lower snake_case file slug

// Lowercase a single byte
static inline u8 cstr__isLowerC(u8 c) {
  return ('A' <= c && c <= 'Z') ? (u8)(c + ('a' - 'A')) : c;
}

// Length of a null-terminated string, bounded by MAX_LOOP_ITER
u32 cstr__len(const char* c) {
  if (NULL == c) {
    return 0;
  }
  u32 i = 0;
  for (; i < MAX_LOOP_ITER && 0 != c[i]; i++);
  return i;
}

// Compare the first len bytes of two strings
bool cstr__eqn(u32 len, const char* a, const char* b) {
  if (NULL == a || NULL == b) {
    return false;
  }
  for (u32 i = 0; i < len && i < MAX_LOOP_ITER; i++) {
    if (a[i] != b[i]) {
      return false;
    }
    if (0 == a[i]) {
      return true;  // both ended together
    }
  }
  return true;
}

// Compare two null-terminated strings for equality
bool cstr__eq(const char* a, const char* b) {
  if (NULL == a || NULL == b) {
    return false;
  }
  u32 la = cstr__len(a);
  if (la != cstr__len(b)) {
    return false;
  }
  return cstr__eqn(la, a, b);
}

// Case-insensitive compare of the first len bytes
bool cstr__ieqn(u32 len, const char* a, const char* b) {
  if (NULL == a || NULL == b) {
    return false;
  }
  for (u32 i = 0; i < len && i < MAX_LOOP_ITER; i++) {
    if (cstr__isLowerC((u8)a[i]) != cstr__isLowerC((u8)b[i])) {
      return false;
    }
    if (0 == a[i]) {
      return true;
    }
  }
  return true;
}

// Copy src_len bytes of src into dst, always null-terminating; false when truncated
bool cstr__copyn(char* dst, u32 dst_sz, const char* src, u32 src_len) {
  if (NULL == dst || 0 == dst_sz) {
    return false;
  }
  if (NULL == src) {
    dst[0] = 0;
    return false;
  }
  u32 n = src_len;
  if (n > dst_sz - 1) {
    n = dst_sz - 1;
  }
  for (u32 i = 0; i < n && i < MAX_LOOP_ITER; i++) {
    dst[i] = src[i];
  }
  dst[n] = 0;
  return n == src_len;
}

// Copy src into dst, always null-terminating; false when truncated
bool cstr__copy(char* dst, u32 dst_sz, const char* src) {
  return cstr__copyn(dst, dst_sz, src, cstr__len(src));
}

// Append src to dst, always null-terminating; false when truncated
bool cstr__cat(char* dst, u32 dst_sz, const char* src) {
  if (NULL == dst || 0 == dst_sz) {
    return false;
  }
  u32 at = cstr__len(dst);
  if (at >= dst_sz - 1) {
    return false;
  }
  return cstr__copy(dst + at, dst_sz - at, src);
}

// Does c end with suffix?
bool cstr__endsWith(const char* c, const char* suffix) {
  u32 lc = cstr__len(c), ls = cstr__len(suffix);
  if (ls > lc) {
    return false;
  }
  return cstr__eqn(ls, c + (lc - ls), suffix);
}

// Does hay contain needle? (empty needle always matches)
bool cstr__contains(const char* hay, const char* needle) {
  u32 lh = cstr__len(hay), ln = cstr__len(needle);
  if (0 == ln) {
    return true;
  }
  if (ln > lh) {
    return false;
  }
  for (u32 i = 0; i + ln <= lh && i < MAX_LOOP_ITER; i++) {
    if (cstr__eqn(ln, hay + i, needle)) {
      return true;
    }
  }
  return false;
}

// Case-insensitive substring test (empty needle always matches)
bool cstr__icontains(const char* hay, const char* needle) {
  u32 lh = cstr__len(hay), ln = cstr__len(needle);
  if (0 == ln) {
    return true;
  }
  if (ln > lh) {
    return false;
  }
  for (u32 i = 0; i + ln <= lh && i < MAX_LOOP_ITER; i++) {
    if (cstr__ieqn(ln, hay + i, needle)) {
      return true;
    }
  }
  return false;
}

// Parse an unsigned decimal integer; false unless the whole string is digits
bool cstr__toU32(const char* s, u32* out) {
  if (NULL == s || NULL == out || 0 == s[0]) {
    return false;
  }
  u64 v = 0;
  for (u32 i = 0; 0 != s[i] && i < MAX_LOOP_ITER; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    v = v * 10ULL + (u64)(s[i] - '0');
    if (v > 0xFFFFFFFFULL) {
      return false;
    }
  }
  *out = (u32)v;
  return true;
}

// Strip trailing whitespace in place
void cstr__trimRight(char* c) {
  if (NULL == c) {
    return;
  }
  u32 n = cstr__len(c);
  while (n > 0 && (' ' == c[n - 1] || '\n' == c[n - 1] || '\r' == c[n - 1] || '\t' == c[n - 1])) {
    c[--n] = 0;
  }
}

// Reduce src to a safe lower snake_case slug usable as a filename component
bool cstr__slugify(char* dst, u32 dst_sz, const char* src) {
  if (NULL == dst || dst_sz < 2) {
    return false;
  }
  u32 j = 0;
  bool sep = false;  // collapse runs of separators
  for (u32 i = 0; 0 != src[i] && i < MAX_LOOP_ITER && j < dst_sz - 1; i++) {
    u8 c = cstr__isLowerC((u8)src[i]);
    bool keep = ('a' <= c && c <= 'z') || ('0' <= c && c <= '9');
    if (keep) {
      if (sep && j > 0) {
        dst[j++] = '_';
      }
      sep = false;
      if (j < dst_sz - 1) {
        dst[j++] = (char)c;
      }
    } else {
      sep = true;
    }
  }
  dst[j] = 0;
  return j > 0;
}
