#pragma once

#include "../unity.h"  // IWYU pragma: keep

// @class Time
// Function | Purpose
// --- | ---
// Time__sleep_ms(ms) | Sleep for specified milliseconds using nanosleep
// Time__unix_ms() | Get current wall-clock Unix timestamp in milliseconds
// Time__perf_now() | Get high-resolution monotonic time in nanoseconds
// Time__now() | Get monotonic milliseconds elapsed since first call
// Time__ms(ns) | Convert nanoseconds to milliseconds
// Time__ymd(unix_ms, out, out_sz) | Format a timestamp as "YYYYMMDD" (UTC)

// Sleep for specified milliseconds using nanosleep
void Time__sleep_ms(u32 ms) {
  struct timespec req;
  req.tv_sec = ms / 1000;
  req.tv_nsec = (long)(ms % 1000) * 1000000L;
  (void)nanosleep(&req, NULL);
}

// Get current wall-clock Unix timestamp in milliseconds
s64 Time__unix_ms(void) {
  struct timespec ts;
  if (0 != clock_gettime(CLOCK_REALTIME, &ts)) {
    return 0;  // Error case
  }
  return (s64)ts.tv_sec * 1000LL + (s64)(ts.tv_nsec / 1000000L);
}

// Get high-resolution monotonic time in nanoseconds
u64 Time__perf_now(void) {
  struct timespec ts;
  if (0 != clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0;  // Error case
  }
  return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

// Convert nanoseconds to milliseconds
u64 Time__ms(u64 ns) {
  return ns / 1000000ULL;
}

// Get monotonic milliseconds elapsed since first call
u64 Time__now(void) {
  static u64 start_ns = 0;
  if (0 == start_ns) {
    start_ns = Time__perf_now();
  }
  return Time__ms(Time__perf_now() - start_ns);
}

// Format a timestamp as "YYYYMMDD" (UTC); returns false when out is too small
bool Time__ymd(s64 unix_ms, char* out, u32 out_sz) {
  if (NULL == out || out_sz < 9) {
    return false;
  }
  time_t secs = (time_t)(unix_ms / 1000LL);
  struct tm tmv;
  if (NULL == gmtime_r(&secs, &tmv)) {
    return false;
  }
  int n = snprintf(
      out,
      (size_t)out_sz,
      "%04d%02d%02d",
      tmv.tm_year + 1900,
      tmv.tm_mon + 1,
      tmv.tm_mday);
  return n > 0 && (u32)n < out_sz;
}
