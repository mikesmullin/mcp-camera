#pragma once

#include "../unity.h"  // IWYU pragma: keep

// Immediate-mode JSON reader: no AST, no allocation except unescaped strings,
// which are pushed onto a caller-supplied scratch arena.
//
// inspired by:
// - [2025 Tsoding - Immediate JSON Parsing in C](https://www.youtube.com/watch?v=FBpgdSjJ6nQ)
// - c99-server/src/common/Json.c

// @class Json
// Function | Purpose
// --- | ---
// Json__init(json, data, len, scratch) | Bind a parser to an input buffer
// Json__bool(json, out) | Parse a JSON boolean value
// Json__number(json, out) | Parse a JSON number as f64
// Json__u32(json, out) | Parse a JSON number as u32
// Json__string(json, out) | Parse a JSON string, unescaped into the scratch arena
// Json__stringTo(json, dst, dst_sz) | Parse a JSON string into a fixed buffer
// Json__array_begin(json) | Expect an opening square bracket
// Json__array_item(json) | Is another array item available?
// Json__array_end(json) | Expect a closing square bracket
// Json__object_begin(json) | Expect an opening curly brace
// Json__object_key(json, out) | Read the next object key, or false at the brace
// Json__object_end(json) | Expect a closing curly brace
// Json__peek(json) | Look at the next token without consuming it
// Json__skipValue(json) | Consume one whole value, including nested containers
// Json__find(json, path, depth_limit) | (helpers below) seek a key inside the current object

#define JSON_DQUOTE ('"')

// sparse array; maps an ascii char to its 1-symbol token
static JsonTok _JSON_PUNCTS[256] = {
    ['{'] = JSON_OCURLY,
    ['}'] = JSON_CCURLY,
    ['['] = JSON_OBRACKET,
    [']'] = JSON_CBRACKET,
    [','] = JSON_COMMA,
    [':'] = JSON_COLON,
};

// map cstr -> token (for reading literals)
static JsonSym _JSON_SYMBOLS_IN[] = {
    {.token = JSON_TRUE, .symbol = "true"},
    {.token = JSON_FALSE, .symbol = "false"},
    {.token = JSON_NULL, .symbol = "null"},
};

// Bind a parser to an input buffer
void Json__init(Json* json, const char* data, u32 len, Arena* scratch) {
  memset(json, 0, sizeof(*json));
  json->data = data;
  json->len = len;
  json->scratch = scratch;
}

// advance cursor while it points at whitespace
static void _Json__skipWhitespace(Json* json) {
  while (json->cur < json->len && isspace((u8)json->data[json->cur])) {
    json->cur++;
  }
}

// decode \uXXXX into UTF-8; returns bytes written (0 on malformed input)
static u32 _Json__utf8(Json* json, u32* cur, char* out) {
  if (*cur + 4 > json->len) {
    return 0;
  }
  u32 cp = 0;
  for (u32 k = 0; k < 4; k++) {
    char c = json->data[*cur + k];
    u32 d;
    if ('0' <= c && c <= '9') {
      d = (u32)(c - '0');
    } else if ('a' <= c && c <= 'f') {
      d = (u32)(c - 'a') + 10;
    } else if ('A' <= c && c <= 'F') {
      d = (u32)(c - 'A') + 10;
    } else {
      return 0;
    }
    cp = (cp << 4) | d;
  }
  *cur += 4;
  // NOTE: lone surrogates are emitted as U+FFFD rather than failing the parse
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    cp = 0xFFFD;
  }
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

// read a quoted string (cursor is just past the opening quote) into scratch
static bool _Json__readString(Json* json) {
  u32 cur = json->cur;
  // first pass: measure the unescaped length so we can reserve exactly
  u32 need = 0;
  bool closed = false;
  for (u32 i = 0; cur < json->len && i < MAX_LOOP_ITER; i++) {
    char c = json->data[cur++];
    if (JSON_DQUOTE == c) {
      closed = true;
      break;
    }
    if ('\\' != c) {
      need++;
      continue;
    }
    if (cur >= json->len) {
      break;
    }
    char e = json->data[cur++];
    if ('u' == e) {
      need += 3;  // worst case for the BMP
      cur += 4;
    } else {
      need += 1;
    }
  }
  if (!closed) {
    json->failed = true;
    LOG_ERRORF("JSON: unterminated string at byte %u", json->cur);
    return false;
  }

  char* dst = (char*)Arena__push(json->scratch, (u64)need + 1ULL);
  u32 j = 0;
  cur = json->cur;
  for (u32 i = 0; cur < json->len && i < MAX_LOOP_ITER; i++) {
    char c = json->data[cur++];
    if (JSON_DQUOTE == c) {
      break;
    }
    if ('\\' != c) {
      dst[j++] = c;
      continue;
    }
    char e = json->data[cur++];
    switch (e) {
      case 'n':
        dst[j++] = '\n';
        break;
      case 't':
        dst[j++] = '\t';
        break;
      case 'r':
        dst[j++] = '\r';
        break;
      case 'b':
        dst[j++] = '\b';
        break;
      case 'f':
        dst[j++] = '\f';
        break;
      case '/':
        dst[j++] = '/';
        break;
      case '\\':
        dst[j++] = '\\';
        break;
      case JSON_DQUOTE:
        dst[j++] = (char)JSON_DQUOTE;
        break;
      case 'u': {
        u32 n = _Json__utf8(json, &cur, dst + j);
        if (0 == n) {
          json->failed = true;
          LOG_ERRORF("JSON: bad \\u escape at byte %u", cur);
          return false;
        }
        j += n;
        break;
      }
      default:
        json->failed = true;
        LOG_ERRORF("JSON: unknown escape '\\%c'", e);
        return false;
    }
  }
  dst[j] = 0;
  json->cur = cur;
  json->str.str = dst;
  json->str.len = j;
  json->token = JSON_STRING;
  return true;
}

// parse a JSON number at the cursor without relying on a NUL terminator
static bool _Json__readNumber(Json* json) {
  u32 start = json->cur;
  u32 cur = start;
  if (cur < json->len && ('-' == json->data[cur] || '+' == json->data[cur])) {
    cur++;
  }
  u32 digits = 0;
  while (cur < json->len && json->data[cur] >= '0' && json->data[cur] <= '9') {
    cur++;
    digits++;
  }
  if (cur < json->len && '.' == json->data[cur]) {
    cur++;
    while (cur < json->len && json->data[cur] >= '0' && json->data[cur] <= '9') {
      cur++;
      digits++;
    }
  }
  if (0 == digits) {
    return false;
  }
  if (cur < json->len && ('e' == json->data[cur] || 'E' == json->data[cur])) {
    u32 save = cur;
    cur++;
    if (cur < json->len && ('-' == json->data[cur] || '+' == json->data[cur])) {
      cur++;
    }
    u32 ed = 0;
    while (cur < json->len && json->data[cur] >= '0' && json->data[cur] <= '9') {
      cur++;
      ed++;
    }
    if (0 == ed) {
      cur = save;  // 'e' was not part of the number
    }
  }

  // copy the span out so strtod() sees a terminated buffer
  char tmp[64];
  u32 n = cur - start;
  if (n >= sizeof(tmp)) {
    return false;
  }
  (void)cstr__copyn(tmp, (u32)sizeof(tmp), json->data + start, n);
  json->number = strtod(tmp, NULL);
  json->cur = cur;
  json->token = JSON_NUMBER;
  return true;
}

// advance exactly one token, until eof (primary walk fn)
static bool _Json__nextToken(Json* json) {
  if (json->failed) {
    return false;
  }
  _Json__skipWhitespace(json);

  if (json->cur >= json->len) {
    json->token = JSON_EOF;
    return false;
  }

  // 1-symbol token (ie. punctuation)
  JsonTok punct = _JSON_PUNCTS[(u8)json->data[json->cur]];
  if (JSON_INVALID != punct) {
    json->token = punct;
    json->cur++;
    return true;
  }

  // string
  if (JSON_DQUOTE == json->data[json->cur]) {
    json->cur++;
    return _Json__readString(json);
  }

  // literal (true / false / null)
  for (u32 i = 0; i < ARRAYSIZE(_JSON_SYMBOLS_IN); i++) {
    const char* sym = _JSON_SYMBOLS_IN[i].symbol;
    u32 slen = cstr__len(sym);
    if (json->cur + slen <= json->len && cstr__eqn(slen, json->data + json->cur, sym)) {
      json->cur += slen;
      json->token = _JSON_SYMBOLS_IN[i].token;
      return true;
    }
  }

  // number
  if (_Json__readNumber(json)) {
    return true;
  }

  json->token = JSON_INVALID;
  json->failed = true;
  LOG_ERRORF("JSON: invalid token at byte %u", json->cur);
  return false;
}

// Look at the next token without consuming it
JsonTok Json__peek(Json* json) {
  u32 save_cur = json->cur;
  JsonTok save_tok = json->token;
  if (!_Json__nextToken(json)) {
    JsonTok t = json->token;
    json->cur = save_cur;
    json->token = save_tok;
    return t;
  }
  JsonTok t = json->token;
  json->cur = save_cur;
  json->token = save_tok;
  return t;
}

// consume the next token and require it to be `token`
static bool _Json__expect(Json* json, JsonTok token) {
  if (!_Json__nextToken(json)) {
    return false;
  }
  if (json->token != token) {
    json->failed = true;
    LOG_ERRORF("JSON: expected token %d but got %d at byte %u", token, json->token, json->cur);
    return false;
  }
  return true;
}

// Parse a JSON boolean value
bool Json__bool(Json* json, bool* out) {
  if (!_Json__nextToken(json)) {
    return false;
  }
  if (JSON_TRUE == json->token) {
    *out = true;
    return true;
  }
  if (JSON_FALSE == json->token) {
    *out = false;
    return true;
  }
  json->failed = true;
  LOG_ERRORF("JSON: expected boolean at byte %u", json->cur);
  return false;
}

// Parse a JSON number as f64
bool Json__number(Json* json, f64* out) {
  if (!_Json__expect(json, JSON_NUMBER)) {
    return false;
  }
  *out = json->number;
  return true;
}

// Parse a JSON number as u32 (negatives and fractions are rejected)
bool Json__u32(Json* json, u32* out) {
  f64 v = 0.0;
  if (!Json__number(json, &v)) {
    return false;
  }
  if (v < 0.0 || v > 4294967295.0) {
    json->failed = true;
    LOG_ERRORF("JSON: number out of u32 range");
    return false;
  }
  *out = (u32)v;
  return true;
}

// Parse a JSON string, unescaped into the scratch arena
bool Json__string(Json* json, JsonStr* out) {
  if (!_Json__expect(json, JSON_STRING)) {
    return false;
  }
  *out = json->str;
  return true;
}

// Parse a JSON string into a fixed buffer (truncating if necessary)
bool Json__stringTo(Json* json, char* dst, u32 dst_sz) {
  JsonStr s = {0};
  if (!Json__string(json, &s)) {
    return false;
  }
  return cstr__copyn(dst, dst_sz, s.str, s.len);
}

// Expect an opening square bracket
bool Json__array_begin(Json* json) {
  return _Json__expect(json, JSON_OBRACKET);
}

// Is another array item available? Consumes a separating comma if present.
bool Json__array_item(Json* json) {
  u32 save = json->cur;
  if (!_Json__nextToken(json)) {
    return false;
  }
  if (JSON_CBRACKET == json->token) {
    json->cur = save;  // leave the bracket for Json__array_end()
    return false;
  }
  if (JSON_COMMA == json->token) {
    return true;  // next value follows
  }
  json->cur = save;  // first value; rewind so the caller reads it
  return true;
}

// Expect a closing square bracket
bool Json__array_end(Json* json) {
  return _Json__expect(json, JSON_CBRACKET);
}

// Expect an opening curly brace
bool Json__object_begin(Json* json) {
  return _Json__expect(json, JSON_OCURLY);
}

// Read the next object key (consuming its colon), or false at the closing brace
bool Json__object_key(Json* json, JsonStr* out) {
  u32 save = json->cur;
  if (!_Json__nextToken(json)) {
    return false;
  }
  if (JSON_CCURLY == json->token) {
    json->cur = save;  // leave the brace for Json__object_end()
    return false;
  }
  if (JSON_COMMA == json->token) {
    if (!_Json__expect(json, JSON_STRING)) {
      return false;
    }
  } else if (JSON_STRING != json->token) {
    json->failed = true;
    LOG_ERRORF("JSON: expected object key at byte %u", json->cur);
    return false;
  }
  *out = json->str;
  return _Json__expect(json, JSON_COLON);
}

// Expect a closing curly brace
bool Json__object_end(Json* json) {
  return _Json__expect(json, JSON_CCURLY);
}

// Consume one whole value, including nested objects and arrays
bool Json__skipValue(Json* json) {
  if (!_Json__nextToken(json)) {
    return false;
  }
  if (JSON_OCURLY != json->token && JSON_OBRACKET != json->token) {
    return true;  // scalar
  }
  u32 depth = 1;
  for (u32 i = 0; i < MAX_LOOP_ITER && depth > 0; i++) {
    if (!_Json__nextToken(json)) {
      return false;
    }
    if (JSON_OCURLY == json->token || JSON_OBRACKET == json->token) {
      depth++;
    } else if (JSON_CCURLY == json->token || JSON_CBRACKET == json->token) {
      depth--;
    }
  }
  return 0 == depth;
}

// Does a parsed key equal this literal?
static inline bool JsonStr__eq(JsonStr s, const char* lit) {
  u32 n = cstr__len(lit);
  return s.len == n && cstr__eqn(n, s.str, lit);
}
