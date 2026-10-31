#pragma once

#include "../unity.h"  // IWYU pragma: keep

// The preview daemon's record of every frame it wrote.
//
// Frames are transient by contract: when the window retires after its idle
// timeout, every frame NOT marked persist is deleted. This is what makes
// camera_look safe to call in a polling loop -- the agent can look as often as
// it likes without filling the disk, and keeps a frame only by asking.
//
// Persisted frames are still recorded, so the summary can report what survived.

// @class Registry
// Function | Purpose
// --- | ---
// Registry__init(r) | Reset the registry
// Registry__add(r, path, persist) | Record a written frame (updates an existing path)
// Registry__sweep(r) | Delete every non-persisted frame, return how many went
// Registry__counts(r, out_kept, out_transient) | Tally what is being tracked

typedef struct {
  char path[MAX_PATH];
  bool persist;
  bool swept;  // already deleted; do not count or re-unlink it
} TrackedFile;

typedef struct {
  TrackedFile items[MAX_TRACKED_FILES];
  u32 ct;
  u32 dropped;  // frames we could not track because the table was full
} Registry;

// Reset the registry
void Registry__init(Registry* r) {
  memset(r, 0, sizeof(*r));
}

// Record a written frame. Re-writing the same path updates its persist flag,
// and persist is sticky: once a caller asks to keep a path, a later transient
// write to it must not schedule it for deletion.
bool Registry__add(Registry* r, const char* path, bool persist) {
  for (u32 i = 0; i < r->ct && i < MAX_TRACKED_FILES; i++) {
    if (cstr__eq(r->items[i].path, path)) {
      r->items[i].persist = r->items[i].persist || persist;
      r->items[i].swept = false;  // the path exists again
      return true;
    }
  }
  if (r->ct >= MAX_TRACKED_FILES) {
    // A full table means we lose the ability to clean this frame up. Say so
    // loudly rather than silently leaking files into /tmp.
    r->dropped++;
    LOG_ERRORF("registry full at %d frames; %s will not be cleaned up", MAX_TRACKED_FILES, path);
    return false;
  }
  (void)cstr__copy(r->items[r->ct].path, MAX_PATH, path);
  r->items[r->ct].persist = persist;
  r->items[r->ct].swept = false;
  r->ct++;
  return true;
}

// Tally what is being tracked
void Registry__counts(const Registry* r, u32* out_kept, u32* out_transient) {
  u32 kept = 0, transient = 0;
  for (u32 i = 0; i < r->ct && i < MAX_TRACKED_FILES; i++) {
    if (r->items[i].persist) {
      kept++;
    } else {
      transient++;
    }
  }
  if (NULL != out_kept) {
    *out_kept = kept;
  }
  if (NULL != out_transient) {
    *out_transient = transient;
  }
}

// Delete every non-persisted frame, return how many went.
// Entries are marked rather than removed, so a second sweep is a no-op that
// reports 0 instead of re-counting (or re-deleting) paths already handled --
// which matters because a later grab may have recreated one of those paths.
u32 Registry__sweep(Registry* r) {
  u32 gone = 0;
  for (u32 i = 0; i < r->ct && i < MAX_TRACKED_FILES; i++) {
    if (r->items[i].persist || r->items[i].swept) {
      continue;
    }
    if (File__unlink(r->items[i].path)) {
      gone++;
      r->items[i].swept = true;
      LOG_DEBUGF("removed transient frame %s", r->items[i].path);
    } else {
      LOG_ERRORF("could not remove transient frame %s", r->items[i].path);
    }
  }
  u32 kept = 0;
  Registry__counts(r, &kept, NULL);
  LOG_INFOF(
      "swept %lu transient frames, kept %lu persisted",
      (unsigned long)gone,
      (unsigned long)kept);
  return gone;
}
