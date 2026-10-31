// Shared Compilation Unit core (Unity Build) ------------------
// Included by both src/mcp.c (camera_mcp) and src/preview.c (camera_preview).
#pragma once

// ---
// Global Dependencies

#include <errno.h>  // IWYU pragma: keep // errno, EEXIST, EAGAIN
#include <signal.h>  // IWYU pragma: keep // signal()
#include <stdarg.h>  // IWYU pragma: keep // va_list
#include <stdio.h>  // IWYU pragma: keep // snprintf()
#include <stdlib.h>  // IWYU pragma: keep // malloc(), exit(), getenv(), realpath()

// ---
// Types

#include <stddef.h>  // IWYU pragma: keep // NULL, offsetof()

// fixed-width types
#include <stdint.h>
// aliases
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

// Booleans
#include <stdbool.h>  // IWYU pragma: keep // typedef bool, #define true (1), false (0)

// cstr
#include <ctype.h>  // IWYU pragma: keep // isspace()
#include <string.h>  // IWYU pragma: keep // memset()

// Arrays
#define ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))

// ---
// Fixed Upper Bounds (every loop in this project has one)

#define MAX_CAMERAS (16)  // /dev/videoN nodes considered
#define MAX_PATH (512)  // filesystem path bytes
#define MAX_CAM_NAME (160)  // "{index}__{card}__{serial}" bytes
#define MAX_FRAME_W (1920)  // widest capture accepted
#define MAX_FRAME_H (1080)  // tallest capture accepted
#define MAX_TRACKED_FILES (256)  // frames the preview registers for cleanup
#define MAX_JSON_BYTES (1u << 16)  // one JSON-RPC line / IPC line
#define MAX_HTTP_BODY (1u << 22)  // 4 MiB response ceiling
#define MAX_LOOP_ITER (1u << 20)  // generic runaway guard

// ---
// Assertions & Logging

// allow intercept from unit test
bool __asserted = false;
bool __expect_assert = false;

static const char* S_CONSOLE_LOG3 = "Assertion failed: %s\n  at %s:%u\n";

#define LOG_INFOF(s, ...) Console__error("[camera] " s "\n", ##__VA_ARGS__)
#define LOG_ERRORF(s, ...) Console__error("[camera] ERROR " s "\n", ##__VA_ARGS__)

#ifdef DEBUG_SLOW
#define LOG_DEBUGF(s, ...) Console__error("[camera] " s "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUGF(s, ...)
#endif

// NOTE: assertions stay enabled in release; this program drives hardware and
// a silent out-of-bounds is worse than a loud abort.
#define ASSERT(cond)                                           \
  if (!(cond)) {                                               \
    Console__abort(S_CONSOLE_LOG3, #cond, __FILE__, __LINE__); \
  }
#define ASSERT_CONTEXT(cond, ctx, ...)                                                                                  \
  if (!(cond)) {                                                                                                        \
    Console__abort("Assertion failed: %s\n  at %s:%u\n  Context: " ctx "\n", #cond, __FILE__, __LINE__, ##__VA_ARGS__); \
  }

#include "common/Log.c"  // IWYU pragma: keep

// ---
// Time

#include <time.h>  // IWYU pragma: keep

#include "common/Time.c"  // IWYU pragma: keep

// ---
// Arena

typedef struct {
  u8* buf;
  u8* pos;
  u8* end;
} Arena;

#include "common/Arena.c"  // IWYU pragma: keep

// ---
// Strings

#include "common/String.c"  // IWYU pragma: keep

// ---
// Base64

#include "common/Base64.c"  // IWYU pragma: keep

// ---
// JSON (reader)

typedef enum {
  JSON_INVALID,
  JSON_EOF,

  // Punctuation
  JSON_OCURLY,
  JSON_CCURLY,
  JSON_OBRACKET,
  JSON_CBRACKET,
  JSON_COMMA,
  JSON_COLON,

  // Literals
  JSON_TRUE,
  JSON_FALSE,
  JSON_NULL,

  // Types
  JSON_STRING,
  JSON_NUMBER,
  JSON_BOOL,

  JSON_COUNT,
} JsonTok;  // JSON Token

typedef struct {
  JsonTok token;
  const char* symbol;
} JsonSym;  // JSON Symbol

// a view into the parser's input buffer (never owns memory)
typedef struct {
  const char* str;
  u32 len;
} JsonStr;

// iterator
typedef struct {
  const char* data;  // parser input
  u32 len;  // parser boundary
  u32 cur;  // read cursor

  // current token
  JsonTok token;  // kind
  f64 number;  // number value (when token == JSON_NUMBER)
  JsonStr str;  // string value (when token == JSON_STRING), unescaped into scratch

  Arena* scratch;  // where unescaped strings land
  bool failed;  // sticky parse error
} Json;

#include "common/Json.c"  // IWYU pragma: keep

// ---
// JSON (writer)

typedef struct {
  char* buf;
  u32 cap;
  u32 len;
  bool overflow;  // sticky: buf was too small
  bool need_comma;  // a value was already written at this depth
} JsonW;  // JSON Writer

#include "common/JsonW.c"  // IWYU pragma: keep

// ---
// Files

#include "common/File.c"  // IWYU pragma: keep

// ---
// JPEG encoder

#include "common/Jpeg.c"  // IWYU pragma: keep

// ---
// Cameras (V4L2 enumeration)

typedef struct {
  u32 index;  // N of /dev/videoN
  char path[MAX_PATH];  // "/dev/video0"
  char card[64];  // "UVC Camera (046d:0821)"
  char serial[64];  // "6A7559A0" ("" when unknown)
  char name[MAX_CAM_NAME];  // "0__UVC Camera (046d:0821)__6A7559A0"
} Camera;

typedef struct {
  Camera items[MAX_CAMERAS];
  u32 ct;
} CameraList;

#include "app/CamId.c"  // IWYU pragma: keep

// ---
// IPC (camera_mcp <-> camera_preview, newline-delimited JSON over a UDS)

// Request kinds the preview daemon serves.
typedef enum {
  IPC_REQ_NONE,
  IPC_REQ_PING,  // readiness probe
  IPC_REQ_GRAB,  // freeze + write a frame, reply with its path
  IPC_REQ_DIE,  // shut the window down now
} IpcReqKind;

typedef struct {
  IpcReqKind kind;
  char file[MAX_PATH];  // "" = daemon picks a transient path
  bool persist;  // survives idle cleanup
  u32 quality;  // JPEG quality 1..100
} IpcReq;

typedef struct {
  bool ok;
  char path[MAX_PATH];
  u32 width, height;
  u32 bytes;
  s64 captured_at;  // unix ms
  bool persist;
  char error[160];
} IpcRes;

#include "app/Ipc.c"  // IWYU pragma: keep
