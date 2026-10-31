#pragma once

#include <sys/stat.h>  // IWYU pragma: keep // mkdir(), stat()
#include <unistd.h>  // IWYU pragma: keep // unlink(), access()

#include "../unity.h"  // IWYU pragma: keep

// @class File
// Function | Purpose
// --- | ---
// File__writeAll(path, data, len) | Write a whole buffer to path, replacing it
// File__readAll(path, arena, out, out_len) | Read a whole file into the arena
// File__size(path, out) | Stat a file's size in bytes
// File__exists(path) | Does path exist?
// File__unlink(path) | Delete a file, tolerating absence
// File__mkdirp(path) | Create a directory and all missing parents
// File__dirnameOf(dst, dst_sz, path) | Copy the directory portion of path
// File__mkdirpFor(path) | Create the parent directory chain of a file path

// Write a whole buffer to path, replacing it
bool File__writeAll(const char* path, const u8* data, u64 len) {
  FILE* f = fopen(path, "wb");
  if (NULL == f) {
    LOG_ERRORF("cannot open for write: %s", path);
    return false;
  }
  u64 wrote = 0 == len ? 0 : (u64)fwrite(data, 1, (size_t)len, f);
  if (0 != fclose(f)) {
    LOG_ERRORF("cannot close after write: %s", path);
    return false;
  }
  if (wrote != len) {
    LOG_ERRORF(
        "short write: %s (%llu of %llu)",
        path,
        (unsigned long long)wrote,
        (unsigned long long)len);
    return false;
  }
  return true;
}

// Stat a file's size in bytes
bool File__size(const char* path, u64* out) {
  struct stat st;
  if (0 != stat(path, &st)) {
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    return false;
  }
  *out = (u64)st.st_size;
  return true;
}

// Does path exist?
bool File__exists(const char* path) {
  return 0 == access(path, F_OK);
}

// Delete a file, tolerating absence
bool File__unlink(const char* path) {
  if (0 == unlink(path)) {
    return true;
  }
  return !File__exists(path);
}

// Read a whole file into the arena; out is NUL-terminated for convenience
bool File__readAll(const char* path, Arena* arena, u8** out, u64* out_len) {
  u64 sz = 0;
  if (!File__size(path, &sz)) {
    LOG_ERRORF("not a readable regular file: %s", path);
    return false;
  }
  FILE* f = fopen(path, "rb");
  if (NULL == f) {
    LOG_ERRORF("cannot open for read: %s", path);
    return false;
  }
  u8* buf = (u8*)Arena__push(arena, sz + 1ULL);
  u64 got = 0 == sz ? 0 : (u64)fread(buf, 1, (size_t)sz, f);
  (void)fclose(f);
  if (got != sz) {
    LOG_ERRORF(
        "short read: %s (%llu of %llu)",
        path,
        (unsigned long long)got,
        (unsigned long long)sz);
    return false;
  }
  buf[sz] = 0;
  *out = buf;
  *out_len = sz;
  return true;
}

// Create a directory and all missing parents (mode 0700; frames can be private)
bool File__mkdirp(const char* path) {
  char tmp[MAX_PATH];
  if (!cstr__copy(tmp, (u32)sizeof(tmp), path)) {
    LOG_ERRORF("path too long to create: %s", path);
    return false;
  }
  u32 n = cstr__len(tmp);
  if (0 == n) {
    return false;
  }
  // walk each separator, creating prefixes as we go
  for (u32 i = 1; i < n && i < MAX_LOOP_ITER; i++) {
    if ('/' != tmp[i]) {
      continue;
    }
    tmp[i] = 0;
    if (0 != mkdir(tmp, 0700) && EEXIST != errno) {
      LOG_ERRORF("cannot create directory: %s", tmp);
      return false;
    }
    tmp[i] = '/';
  }
  if (0 != mkdir(tmp, 0700) && EEXIST != errno) {
    LOG_ERRORF("cannot create directory: %s", tmp);
    return false;
  }
  return true;
}

// Copy the directory portion of path (no trailing slash; "." when none)
bool File__dirnameOf(char* dst, u32 dst_sz, const char* path) {
  u32 n = cstr__len(path);
  u32 cut = 0;
  for (u32 i = 0; i < n && i < MAX_LOOP_ITER; i++) {
    if ('/' == path[i]) {
      cut = i;
    }
  }
  if (0 == cut) {
    // either "/file" (root) or a bare relative name
    return cstr__copy(dst, dst_sz, '/' == path[0] ? "/" : ".");
  }
  return cstr__copyn(dst, dst_sz, path, cut);
}

// Create the parent directory chain of a file path
bool File__mkdirpFor(const char* path) {
  char dir[MAX_PATH];
  if (!File__dirnameOf(dir, (u32)sizeof(dir), path)) {
    return false;
  }
  if (cstr__eq(dir, ".") || cstr__eq(dir, "/")) {
    return true;
  }
  return File__mkdirp(dir);
}
