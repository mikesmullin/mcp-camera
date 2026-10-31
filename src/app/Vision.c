#pragma once

#include "../unity.h"  // IWYU pragma: keep

// Clients for the two local inference services. The request shapes are the
// same ones mcp-vision uses, so both servers can share one model process:
//
//   OCR   POST {OCR_URL}/ocr                  {"op":"read","image":"<base64>"}
//         -> {"ok":true,"regions":[{bounds:[x1,y1,x2,y2],text,score}],width,height}
//   VLM   POST {VLM_URL}/v1/chat/completions  OpenAI chat with an image_url part
//         -> {"choices":[{"message":{"content":"..."}}]}
//
// OCR answers "what text is in frame" and the VLM answers "what is in frame".
// camera_look asks both, because they are different questions and the agent
// usually wants whichever one has signal.
//
// SECURITY: text returned from either backend is untrusted content read off a
// camera. It is reported as data and must never be treated as instructions.

#define VISION_MAX_PROSE (4096)
#define VISION_MAX_OCR_TEXT (4096)
#define VISION_MAX_OCR_JSON (16384)
#define VISION_MAX_REGIONS (100)
#define VISION_TIMEOUT_MS (120000)

// @class Vision
// Function | Purpose
// --- | ---
// Vision__ocrUrl() | Configured OCR base url
// Vision__vlmUrl() | Configured VLM base url
// Vision__init(out) | Zero a result
// Vision__describe(jpeg, len, prompt, arena, out) | Ask the VLM about the frame
// Vision__ocr(jpeg, len, query, arena, out) | Read text regions from the frame

typedef struct {
  bool vlm_ok;
  char prose[VISION_MAX_PROSE];  // the VLM's answer
  char vlm_error[160];

  bool ocr_ok;
  u32 ocr_ct;  // regions kept after the query filter
  u32 ocr_total;  // regions the backend returned
  char ocr_text[VISION_MAX_OCR_TEXT];  // kept regions joined by newlines
  char ocr_json[VISION_MAX_OCR_JSON];  // kept regions as a JSON array
  char ocr_error[160];
} VisionResult;

// Configured OCR base url
static const char* Vision__ocrUrl(void) {
  const char* v = getenv("CAMERA_OCR_URL");
  if (NULL == v || 0 == v[0]) {
    v = getenv("VISION_OCR_URL");  // match mcp-vision's name
  }
  return (NULL != v && 0 != v[0]) ? v : "http://127.0.0.1:1239";
}

// Configured VLM base url
static const char* Vision__vlmUrl(void) {
  const char* v = getenv("CAMERA_VLM_URL");
  if (NULL == v || 0 == v[0]) {
    v = getenv("VISION_LIQUID_URL");  // match mcp-vision's name
  }
  return (NULL != v && 0 != v[0]) ? v : "http://127.0.0.1:1246";
}

// Zero a result
void Vision__init(VisionResult* out) {
  memset(out, 0, sizeof(*out));
}

// base64-encode a frame into the arena
static bool _Vision__b64(const u8* jpeg, u64 len, Arena* arena, char** out, u64* out_len) {
  u64 need = Base64__encodedLen(len);
  char* b64 = (char*)Arena__push(arena, need);
  if (!Base64__encode(b64, need, jpeg, len)) {
    return false;
  }
  *out = b64;
  *out_len = need - 1ULL;
  return true;
}

// POST a prepared body to one of the configured services
static bool _Vision__call(
    const char* base_url,
    const char* endpoint,
    const char* body,
    u64 body_len,
    Arena* arena,
    HttpRes* res,
    char* err,
    u32 err_sz) {
  char host[128], base_path[128], path[256];
  u16 port = 80;
  if (!Http__parseUrl(
          base_url,
          host,
          (u32)sizeof(host),
          &port,
          base_path,
          (u32)sizeof(base_path))) {
    (void)cstr__copy(err, err_sz, "backend url is malformed");
    return false;
  }
  // a base url may carry a prefix path; join it with the endpoint
  if (cstr__eq(base_path, "/")) {
    (void)cstr__copy(path, (u32)sizeof(path), endpoint);
  } else {
    (void)cstr__copy(path, (u32)sizeof(path), base_path);
    if (cstr__endsWith(path, "/")) {
      path[cstr__len(path) - 1] = 0;
    }
    (void)cstr__cat(path, (u32)sizeof(path), endpoint);
  }

  if (!Http__post(host, port, path, (const u8*)body, body_len, VISION_TIMEOUT_MS, arena, res)) {
    (void)snprintf(err, (size_t)err_sz, "cannot reach %s%s", base_url, endpoint);
    return false;
  }
  if (200 != res->status) {
    (void)snprintf(
        err,
        (size_t)err_sz,
        "%s%s returned HTTP %lu",
        base_url,
        endpoint,
        (unsigned long)res->status);
    return false;
  }
  return true;
}

// Ask the VLM to describe the frame in prose
bool Vision__describe(
    const u8* jpeg, u64 len, const char* prompt, Arena* arena, VisionResult* out) {
  char* b64 = NULL;
  u64 b64_len = 0;
  if (!_Vision__b64(jpeg, len, arena, &b64, &b64_len)) {
    (void)cstr__copy(out->vlm_error, (u32)sizeof(out->vlm_error), "cannot encode frame");
    return false;
  }

  // the body is dominated by the data url; reserve for it plus the prompt
  u64 cap = b64_len + (u64)cstr__len(prompt) * 2ULL + 1024ULL;
  char* body = (char*)Arena__push(arena, cap);
  JsonW w;
  JsonW__init(&w, body, (u32)(cap > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : cap));
  JsonW__objBegin(&w);
  JsonW__kstr(&w, "model", "default");
  JsonW__key(&w, "messages");
  JsonW__arrBegin(&w);
  JsonW__objBegin(&w);
  JsonW__kstr(&w, "role", "user");
  JsonW__key(&w, "content");
  JsonW__arrBegin(&w);
  JsonW__objBegin(&w);
  JsonW__kstr(&w, "type", "image_url");
  JsonW__key(&w, "image_url");
  JsonW__objBegin(&w);
  // the data url is far too large to pass as one argument, so stream it
  JsonW__key(&w, "url");
  JsonW__strOpen(&w);
  JsonW__strPart(&w, "data:image/jpeg;base64,", 23);
  JsonW__strPart(&w, b64, (u32)b64_len);
  JsonW__strClose(&w);
  JsonW__objEnd(&w);
  JsonW__objEnd(&w);
  JsonW__objBegin(&w);
  JsonW__kstr(&w, "type", "text");
  JsonW__kstr(&w, "text", prompt);
  JsonW__objEnd(&w);
  JsonW__arrEnd(&w);
  JsonW__objEnd(&w);
  JsonW__arrEnd(&w);
  JsonW__ku32(&w, "max_tokens", 256);
  JsonW__ku32(&w, "temperature", 0);
  JsonW__objEnd(&w);
  if (!JsonW__ok(&w)) {
    (void)cstr__copy(out->vlm_error, (u32)sizeof(out->vlm_error), "request body too large");
    return false;
  }

  HttpRes res = {0};
  if (!_Vision__call(
          Vision__vlmUrl(),
          "/v1/chat/completions",
          body,
          w.len,
          arena,
          &res,
          out->vlm_error,
          (u32)sizeof(out->vlm_error))) {
    return false;
  }

  // walk to choices[0].message.content
  Json j;
  Json__init(&j, res.body, res.body_len, arena);
  if (!Json__object_begin(&j)) {
    (void)cstr__copy(out->vlm_error, (u32)sizeof(out->vlm_error), "response was not an object");
    return false;
  }
  bool found = false;
  JsonStr key = {0};
  for (u32 i = 0; i < MAX_LOOP_ITER && Json__object_key(&j, &key); i++) {
    if (!JsonStr__eq(key, "choices")) {
      if (!Json__skipValue(&j)) {
        break;
      }
      continue;
    }
    if (!Json__array_begin(&j)) {
      break;
    }
    // only the first choice matters
    if (Json__array_item(&j)) {
      if (!Json__object_begin(&j)) {
        break;
      }
      JsonStr ck = {0};
      for (u32 k = 0; k < MAX_LOOP_ITER && Json__object_key(&j, &ck); k++) {
        if (!JsonStr__eq(ck, "message")) {
          if (!Json__skipValue(&j)) {
            break;
          }
          continue;
        }
        if (!Json__object_begin(&j)) {
          break;
        }
        JsonStr mk = {0};
        for (u32 m = 0; m < MAX_LOOP_ITER && Json__object_key(&j, &mk); m++) {
          if (JsonStr__eq(mk, "content")) {
            if (Json__stringTo(&j, out->prose, (u32)sizeof(out->prose))) {
              found = true;
            }
            continue;
          }
          if (!Json__skipValue(&j)) {
            break;
          }
        }
        (void)Json__object_end(&j);
      }
      (void)Json__object_end(&j);
    }
    break;  // stop once choices has been consumed
  }

  if (!found) {
    (void)cstr__copy(
        out->vlm_error,
        (u32)sizeof(out->vlm_error),
        "response had no choices[0].message.content");
    return false;
  }
  out->vlm_ok = true;
  return true;
}

// one OCR text region, in input-image pixels
typedef struct {
  u32 x, y, w, h;
  f64 score;
  char text[512];
} OcrRegion;

// parse one {"bounds":[..],"text":"..","score":n} object
static bool _Vision__ocrRegion(Json* j, OcrRegion* out) {
  memset(out, 0, sizeof(*out));
  if (!Json__object_begin(j)) {
    return false;
  }
  u32 b[4] = {0, 0, 0, 0};
  bool have_bounds = false;
  JsonStr key = {0};
  for (u32 i = 0; i < MAX_LOOP_ITER && Json__object_key(j, &key); i++) {
    if (JsonStr__eq(key, "bounds")) {
      if (!Json__array_begin(j)) {
        return false;
      }
      u32 n = 0;
      for (u32 k = 0; k < 8 && Json__array_item(j); k++) {
        f64 v = 0.0;
        if (!Json__number(j, &v)) {
          return false;
        }
        if (n < 4) {
          b[n++] = v < 0.0 ? 0u : (u32)v;
        }
      }
      if (!Json__array_end(j)) {
        return false;
      }
      have_bounds = 4 == n;
    } else if (JsonStr__eq(key, "text")) {
      if (!Json__stringTo(j, out->text, (u32)sizeof(out->text))) {
        return false;
      }
    } else if (JsonStr__eq(key, "score")) {
      if (!Json__number(j, &out->score)) {
        return false;
      }
    } else if (!Json__skipValue(j)) {
      return false;
    }
  }
  if (!Json__object_end(j)) {
    return false;
  }
  if (!have_bounds) {
    return false;
  }
  // bounds are [x1,y1,x2,y2]; report them as x/y/w/h
  out->x = b[0];
  out->y = b[1];
  out->w = b[2] > b[0] ? b[2] - b[0] : 0;
  out->h = b[3] > b[1] ? b[3] - b[1] : 0;
  return true;
}

// Read text regions from the frame, optionally filtered by a case-insensitive query
bool Vision__ocr(const u8* jpeg, u64 len, const char* query, Arena* arena, VisionResult* out) {
  char* b64 = NULL;
  u64 b64_len = 0;
  if (!_Vision__b64(jpeg, len, arena, &b64, &b64_len)) {
    (void)cstr__copy(out->ocr_error, (u32)sizeof(out->ocr_error), "cannot encode frame");
    return false;
  }

  // {"op":"read","image":"<b64>"} -- base64 needs no JSON escaping
  u64 cap = b64_len + 64ULL;
  char* body = (char*)Arena__push(arena, cap);
  int n = snprintf(body, (size_t)cap, "{\"op\":\"read\",\"image\":\"%s\"}", b64);
  if (n <= 0 || (u64)n >= cap) {
    (void)cstr__copy(out->ocr_error, (u32)sizeof(out->ocr_error), "request body too large");
    return false;
  }

  HttpRes res = {0};
  if (!_Vision__call(
          Vision__ocrUrl(),
          "/ocr",
          body,
          (u64)n,
          arena,
          &res,
          out->ocr_error,
          (u32)sizeof(out->ocr_error))) {
    return false;
  }

  JsonW w;
  JsonW__init(&w, out->ocr_json, (u32)sizeof(out->ocr_json));
  JsonW__arrBegin(&w);

  Json j;
  Json__init(&j, res.body, res.body_len, arena);
  if (!Json__object_begin(&j)) {
    (void)cstr__copy(out->ocr_error, (u32)sizeof(out->ocr_error), "response was not an object");
    return false;
  }
  bool saw_regions = false;
  JsonStr key = {0};
  for (u32 i = 0; i < MAX_LOOP_ITER && Json__object_key(&j, &key); i++) {
    if (!JsonStr__eq(key, "regions")) {
      if (!Json__skipValue(&j)) {
        break;
      }
      continue;
    }
    if (!Json__array_begin(&j)) {
      break;
    }
    saw_regions = true;
    for (u32 k = 0; k < VISION_MAX_REGIONS && Json__array_item(&j); k++) {
      OcrRegion r;
      if (!_Vision__ocrRegion(&j, &r)) {
        break;
      }
      out->ocr_total++;
      if (0 == r.text[0] || !cstr__icontains(r.text, query)) {
        continue;  // filtered out by the query
      }
      out->ocr_ct++;
      JsonW__objBegin(&w);
      JsonW__ku32(&w, "x", r.x);
      JsonW__ku32(&w, "y", r.y);
      JsonW__ku32(&w, "w", r.w);
      JsonW__ku32(&w, "h", r.h);
      JsonW__kstr(&w, "text", r.text);
      JsonW__kf64(&w, "score", r.score, 4);
      JsonW__objEnd(&w);
      // join kept text with newlines for the human-readable summary
      if (0 != out->ocr_text[0]) {
        (void)cstr__cat(out->ocr_text, (u32)sizeof(out->ocr_text), "\n");
      }
      (void)cstr__cat(out->ocr_text, (u32)sizeof(out->ocr_text), r.text);
    }
    (void)Json__array_end(&j);
    break;
  }

  JsonW__arrEnd(&w);
  if (!saw_regions) {
    (void)cstr__copy(out->ocr_error, (u32)sizeof(out->ocr_error), "response had no regions array");
    return false;
  }
  if (!JsonW__ok(&w)) {
    // too many regions to serialize; keep the counts and the joined text
    (void)cstr__copy(out->ocr_json, (u32)sizeof(out->ocr_json), "[]");
  }
  out->ocr_ok = true;
  return true;
}
