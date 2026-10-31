#pragma once

#include "../unity.h"  // IWYU pragma: keep

// IMPORTANT: in camera_mcp, stdout is reserved for the MCP JSON-RPC framing.
// Every diagnostic in this project therefore goes to stderr.

// @class Console
// Function | Purpose
// --- | ---
// Console__init(void) | Disable output buffering for stdout and stderr
// Console__trapAssert(void) | Enable assertion trapping for unit tests
// Console__didAssert(void) | Check if an assertion was triggered during testing
// Console__resetAssert(void) | Reset assertion trap state to default
// Console__out(txt, ...) | Print formatted message to stdout (MCP protocol only)
// Console__error(txt, ...) | Print formatted diagnostic to stderr
// Console__abort(txt, ...) | Print formatted message to stderr and abort program

// Disable output buffering for stdout and stderr
void Console__init(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
}

// Enable assertion trapping for unit tests
void Console__trapAssert(void) {
  __expect_assert = true;
}

// Check if an assertion was triggered during testing
bool Console__didAssert(void) {
  return __asserted;
}

// Reset assertion trap state to default
void Console__resetAssert(void) {
  __expect_assert = false;
  __asserted = false;
}

// Print formatted message to stdout (reserved for MCP protocol frames)
void Console__out(const char* txt, ...) {
  va_list myargs;
  va_start(myargs, txt);
  (void)vfprintf(stdout, txt, myargs);
  va_end(myargs);
  (void)fflush(stdout);
}

// Print formatted diagnostic to stderr
void Console__error(const char* txt, ...) {
  va_list myargs;
  va_start(myargs, txt);
  (void)vfprintf(stderr, txt, myargs);
  va_end(myargs);
  (void)fflush(stderr);
}

// Print formatted message to stderr and abort program
void Console__abort(const char* txt, ...) {
  // allow intercept from unit test
  if (__expect_assert) {
    __asserted = true;
    return;
  }

  va_list myargs;
  va_start(myargs, txt);
  (void)vfprintf(stderr, txt, myargs);
  va_end(myargs);
  (void)fflush(stderr);
  abort();
}
