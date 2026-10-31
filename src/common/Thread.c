#pragma once

#include <pthread.h>  // IWYU pragma: keep // pthread_create()

#include "../unity.h"  // IWYU pragma: keep

// Only the preview daemon is threaded: one capture thread, one IPC thread, and
// the sokol event loop on main. Three threads share two YUYV buffers, so a
// plain mutex is the whole concurrency design.

// @class Mutex
// Function | Purpose
// --- | ---
// Mutex__create(m) | Initialize a mutex
// Mutex__lock(m) | Acquire the lock
// Mutex__unlock(m) | Release the lock
// Mutex__destroy(m) | Release a mutex's resources

// @class Thread
// Function | Purpose
// --- | ---
// Thread__create(t, fn, userdata) | Start a thread
// Thread__join(t) | Wait for a thread to finish
// Thread__detach(t) | Let a thread clean itself up

typedef struct {
  pthread_mutex_t _nix;
} Mutex;

typedef struct {
  pthread_t _nix;
  bool started;
} Thread;

typedef void* (*thread_fn_t)(void*);

// Initialize a mutex
bool Mutex__create(Mutex* m) {
  return 0 == pthread_mutex_init(&m->_nix, NULL);
}

// Acquire the lock
static inline void Mutex__lock(Mutex* m) {
  (void)pthread_mutex_lock(&m->_nix);
}

// Release the lock
static inline void Mutex__unlock(Mutex* m) {
  (void)pthread_mutex_unlock(&m->_nix);
}

// Release a mutex's resources
void Mutex__destroy(Mutex* m) {
  (void)pthread_mutex_destroy(&m->_nix);
}

// Start a thread
bool Thread__create(Thread* t, thread_fn_t fn, void* userdata) {
  t->started = 0 == pthread_create(&t->_nix, NULL, fn, userdata);
  return t->started;
}

// Wait for a thread to finish
void Thread__join(Thread* t) {
  if (t->started) {
    (void)pthread_join(t->_nix, NULL);
    t->started = false;
  }
}

// Let a thread clean itself up
void Thread__detach(Thread* t) {
  if (t->started) {
    (void)pthread_detach(t->_nix);
    t->started = false;
  }
}
