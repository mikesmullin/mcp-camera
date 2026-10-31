#pragma once

#include "../unity.h"  // IWYU pragma: keep

// @class Arena
// Function | Purpose
// --- | ---
// Arena__alloc(sz) | Allocate new arena with given size
// Arena__allocZ(sz) | Allocate + zero-initialize arena
// Arena__zero(arena) | Zero entire arena buffer (SLOW)
// Arena__cap(a) | Get arena capacity in bytes
// Arena__used(a) | Get used bytes in arena
// Arena__remain(a) | Get remaining bytes in arena
// Arena__push(a, sz) | Allocate block from arena (primary function)
// Arena__pushZ(a, sz) | Allocate + zero a block from arena
// Arena__free(a) | Free arena buffer
// Arena__reset(a) | Reset arena position to beginning

// malloc a new arena (used exactly once per lifetime class, per process)
Arena* Arena__alloc(u64 sz) {
  Arena* arena = (Arena*)malloc(sizeof(Arena));
  if (!arena) {
    return NULL;
  }

  arena->buf = (u8*)malloc(sz);
  if (!arena->buf) {
    free(arena);
    return NULL;
  }

  arena->pos = arena->buf;
  arena->end = arena->buf + sz;
  return arena;
}

// zero a whole arena
// WARN: memset() is VERY SLOW!
void Arena__zero(Arena* arena) {
  ASSERT_CONTEXT(arena && arena->buf, "Arena is NULL or uninitialized");
  memset(arena->buf, 0, (size_t)(arena->end - arena->buf));
}

// alloc + zero-init
Arena* Arena__allocZ(u64 sz) {
  Arena* arena = Arena__alloc(sz);
  if (arena) {
    Arena__zero(arena);
  }
  return arena;
}

// return capacity in bytes
static inline u64 Arena__cap(Arena* a) {
  ASSERT_CONTEXT(a && a->buf, "Arena is NULL or uninitialized");
  return (u64)(a->end - a->buf);
}

// return used in bytes
static inline u64 Arena__used(Arena* a) {
  ASSERT_CONTEXT(a && a->buf, "Arena is NULL or uninitialized");
  return (u64)(a->pos - a->buf);
}

// return remaining in bytes
static inline u64 Arena__remain(Arena* a) {
  ASSERT_CONTEXT(a && a->buf, "Arena is NULL or uninitialized");
  return (u64)(a->end - a->pos);
}

// reserve a block of memory from the arena (most commonly used function)
void* Arena__push(Arena* a, u64 sz) {
  ASSERT_CONTEXT(a && a->buf, "Arena is NULL or uninitialized");
  // keep allocations 16-byte aligned so any type may be stored
  u64 aligned = (sz + 15ULL) & ~15ULL;
  ASSERT_CONTEXT(
      a->pos + aligned <= a->end,
      "Arena exhausted. requested %llu bytes, available %llu bytes",
      (unsigned long long)aligned,
      (unsigned long long)Arena__remain(a));

  void* result = a->pos;
  a->pos += aligned;
  return result;
}

// reserve + zero a block of memory from the arena
void* Arena__pushZ(Arena* a, u64 sz) {
  void* p = Arena__push(a, sz);
  memset(p, 0, (size_t)sz);
  return p;
}

// free the allocated memory
void Arena__free(Arena* a) {
  if (a) {
    if (a->buf) {
      free(a->buf);
    }
    free(a);
  }
}

// reset arena write ptr to beginning of arena
void Arena__reset(Arena* a) {
  ASSERT_CONTEXT(a && a->buf, "Arena is NULL or uninitialized");
  a->pos = a->buf;
}
