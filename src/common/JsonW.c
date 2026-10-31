#pragma once

#include "../unity.h"  // IWYU pragma: keep

// Append-only JSON writer over a caller-owned buffer. Overflow is sticky and
// never truncates mid-escape: callers check JsonW__ok() once, at the end.
//
// IMPORTANT: output is always compact (no newlines) because both transports in
// this project are newline-delimited.

// @class JsonW
// Function | Purpose
// --- | ---
// JsonW__init(w, buf, cap) | Bind a writer to a buffer
// JsonW__ok(w) | Did every write fit?
// JsonW__objBegin(w) | Open an object
// JsonW__objEnd(w) | Close an object
// JsonW__arrBegin(w) | Open an array
// JsonW__arrEnd(w) | Close an array
// JsonW__key(w, k) | Write an object key
// JsonW__str(w, v) | Write a string value
// JsonW__strn(w, v, len) | Write a string value of known length
// JsonW__u32(w, v) | Write an unsigned integer value
// JsonW__s64(w, v) | Write a signed integer value
// JsonW__f64(w, v, decimals) | Write a fixed-point number value
// JsonW__bool(w, v) | Write a boolean value
// JsonW__null(w) | Write a null value
// JsonW__raw(w, v) | Splice pre-serialized JSON in as a value
// JsonW__kstr / kstrn / ku32 / ks64 / kf64 / kbool / kraw | key+value shorthands

// Bind a writer to a buffer
void JsonW__init(JsonW* w, char* buf, u32 cap) {
  w->buf = buf;
  w->cap = cap;
  w->len = 0;
  w->overflow = false;
  w->need_comma = false;
  if (cap > 0) {
    buf[0] = 0;
  } else {
    w->overflow = true;
  }
}

// Did every write fit?
static inline bool JsonW__ok(JsonW* w) {
  return !w->overflow;
}

// append raw bytes, marking overflow rather than truncating
static void _JsonW__put(JsonW* w, const char* src, u32 n) {
  if (w->overflow) {
    return;
  }
  if (w->len + n + 1 > w->cap) {
    w->overflow = true;
    return;
  }
  for (u32 i = 0; i < n && i < MAX_LOOP_ITER; i++) {
    w->buf[w->len + i] = src[i];
  }
  w->len += n;
  w->buf[w->len] = 0;
}

static void _JsonW__putc(JsonW* w, char c) {
  _JsonW__put(w, &c, 1);
}

// emit a separating comma when a sibling value already exists
static void _JsonW__comma(JsonW* w) {
  if (w->need_comma) {
    _JsonW__putc(w, ',');
  }
  w->need_comma = true;
}

// Open an object
void JsonW__objBegin(JsonW* w) {
  _JsonW__comma(w);
  _JsonW__putc(w, '{');
  w->need_comma = false;
}

// Close an object
void JsonW__objEnd(JsonW* w) {
  _JsonW__putc(w, '}');
  w->need_comma = true;
}

// Open an array
void JsonW__arrBegin(JsonW* w) {
  _JsonW__comma(w);
  _JsonW__putc(w, '[');
  w->need_comma = false;
}

// Close an array
void JsonW__arrEnd(JsonW* w) {
  _JsonW__putc(w, ']');
  w->need_comma = true;
}

// write a JSON-escaped string body, including its surrounding quotes
static void _JsonW__quoted(JsonW* w, const char* v, u32 len) {
  static const char* HEX = "0123456789abcdef";
  _JsonW__putc(w, '"');
  for (u32 i = 0; i < len && i < MAX_LOOP_ITER; i++) {
    u8 c = (u8)v[i];
    switch (c) {
      case '"':
        _JsonW__put(w, "\\\"", 2);
        break;
      case '\\':
        _JsonW__put(w, "\\\\", 2);
        break;
      case '\n':
        _JsonW__put(w, "\\n", 2);
        break;
      case '\r':
        _JsonW__put(w, "\\r", 2);
        break;
      case '\t':
        _JsonW__put(w, "\\t", 2);
        break;
      case '\b':
        _JsonW__put(w, "\\b", 2);
        break;
      case '\f':
        _JsonW__put(w, "\\f", 2);
        break;
      default:
        if (c < 0x20) {
          char esc[6] = {'\\', 'u', '0', '0', HEX[(c >> 4) & 0xF], HEX[c & 0xF]};
          _JsonW__put(w, esc, 6);
        } else {
          // NOTE: bytes >= 0x80 pass through; inputs are already UTF-8
          _JsonW__putc(w, (char)c);
        }
        break;
    }
  }
  _JsonW__putc(w, '"');
}

// Open a string value, to be filled by JsonW__strPart() calls.
// For values too large to pass as one argument, such as a base64 data url.
void JsonW__strOpen(JsonW* w) {
  _JsonW__comma(w);
  _JsonW__putc(w, '"');
}

// Append a chunk to an open string, escaping it like any other string body
void JsonW__strPart(JsonW* w, const char* v, u32 len) {
  if (NULL == v) {
    return;
  }
  for (u32 i = 0; i < len && i < MAX_LOOP_ITER; i++) {
    u8 c = (u8)v[i];
    // the characters base64 and data urls use need no escaping; anything else
    // takes the slow path through the same table the quoted writer uses
    if (c >= 0x20 && '"' != c && '\\' != c) {
      _JsonW__putc(w, (char)c);
      continue;
    }
    switch (c) {
      case '"':
        _JsonW__put(w, "\\\"", 2);
        break;
      case '\\':
        _JsonW__put(w, "\\\\", 2);
        break;
      case '\n':
        _JsonW__put(w, "\\n", 2);
        break;
      case '\r':
        _JsonW__put(w, "\\r", 2);
        break;
      case '\t':
        _JsonW__put(w, "\\t", 2);
        break;
      case '\b':
        _JsonW__put(w, "\\b", 2);
        break;
      case '\f':
        _JsonW__put(w, "\\f", 2);
        break;
      default: {
        static const char* HEX = "0123456789abcdef";
        char esc[6] = {'\\', 'u', '0', '0', HEX[(c >> 4) & 0xF], HEX[c & 0xF]};
        _JsonW__put(w, esc, 6);
        break;
      }
    }
  }
}

// Close a string opened by JsonW__strOpen()
void JsonW__strClose(JsonW* w) {
  _JsonW__putc(w, '"');
}

// Write an object key
void JsonW__key(JsonW* w, const char* k) {
  _JsonW__comma(w);
  _JsonW__quoted(w, k, cstr__len(k));
  _JsonW__putc(w, ':');
  w->need_comma = false;  // the value that follows is not a sibling
}

// Write a string value of known length
void JsonW__strn(JsonW* w, const char* v, u32 len) {
  _JsonW__comma(w);
  _JsonW__quoted(w, NULL == v ? "" : v, NULL == v ? 0 : len);
}

// Write a string value
void JsonW__str(JsonW* w, const char* v) {
  JsonW__strn(w, v, cstr__len(v));
}

// Write an unsigned integer value
void JsonW__u32(JsonW* w, u32 v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v);
  _JsonW__comma(w);
  _JsonW__put(w, tmp, n > 0 ? (u32)n : 0);
}

// Write a signed integer value
void JsonW__s64(JsonW* w, s64 v) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  _JsonW__comma(w);
  _JsonW__put(w, tmp, n > 0 ? (u32)n : 0);
}

// Write a fixed-point number value
void JsonW__f64(JsonW* w, f64 v, u32 decimals) {
  char tmp[48];
  int n = snprintf(tmp, sizeof(tmp), "%.*f", (int)decimals, v);
  _JsonW__comma(w);
  _JsonW__put(w, tmp, n > 0 ? (u32)n : 0);
}

// Write a boolean value
void JsonW__bool(JsonW* w, bool v) {
  _JsonW__comma(w);
  if (v) {
    _JsonW__put(w, "true", 4);
  } else {
    _JsonW__put(w, "false", 5);
  }
}

// Write a null value
void JsonW__null(JsonW* w) {
  _JsonW__comma(w);
  _JsonW__put(w, "null", 4);
}

// Splice pre-serialized JSON in as a value (caller guarantees validity)
void JsonW__raw(JsonW* w, const char* v) {
  _JsonW__comma(w);
  _JsonW__put(w, v, cstr__len(v));
}

// key+value shorthands ---

void JsonW__kstr(JsonW* w, const char* k, const char* v) {
  JsonW__key(w, k);
  JsonW__str(w, v);
}

void JsonW__kstrn(JsonW* w, const char* k, const char* v, u32 len) {
  JsonW__key(w, k);
  JsonW__strn(w, v, len);
}

void JsonW__ku32(JsonW* w, const char* k, u32 v) {
  JsonW__key(w, k);
  JsonW__u32(w, v);
}

void JsonW__ks64(JsonW* w, const char* k, s64 v) {
  JsonW__key(w, k);
  JsonW__s64(w, v);
}

void JsonW__kf64(JsonW* w, const char* k, f64 v, u32 decimals) {
  JsonW__key(w, k);
  JsonW__f64(w, v, decimals);
}

void JsonW__kbool(JsonW* w, const char* k, bool v) {
  JsonW__key(w, k);
  JsonW__bool(w, v);
}

void JsonW__kraw(JsonW* w, const char* k, const char* v) {
  JsonW__key(w, k);
  JsonW__raw(w, v);
}
