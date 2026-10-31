#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// The registry is what makes camera_look's "transient by default" promise real,
// so these scenarios touch the real filesystem rather than mocking it.
#include "../../src/app/Registry.c"  // IWYU pragma: keep

// @describe Registry
// @tag app

#define DIR "/tmp/mcp-camera-test-registry"

// create a file with some content, so unlink has something to remove
static void touch(const char* path) {
  ASSERT(File__mkdirpFor(path));
  static const u8 DATA[4] = {1, 2, 3, 4};
  ASSERT_CONTEXT(File__writeAll(path, DATA, sizeof(DATA)), "could not create %s", path);
  ASSERT(File__exists(path));
}

int main(void) {
  Console__init();

  Registry r;
  char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH];
  (void)snprintf(a, sizeof(a), "%s/transient_a.jpg", DIR);
  (void)snprintf(b, sizeof(b), "%s/keep_b.jpg", DIR);
  (void)snprintf(c, sizeof(c), "%s/transient_c.jpg", DIR);

  // ---
  // Scenario: a sweep removes transients and keeps persisted frames
  {
    Registry__init(&r);
    touch(a);
    touch(b);
    touch(c);
    ASSERT(Registry__add(&r, a, false));
    ASSERT(Registry__add(&r, b, true));
    ASSERT(Registry__add(&r, c, false));

    u32 kept = 0, transient = 0;
    Registry__counts(&r, &kept, &transient);
    ASSERT_CONTEXT(
        1 == kept && 2 == transient,
        "kept=%lu transient=%lu",
        (unsigned long)kept,
        (unsigned long)transient);

    u32 gone = Registry__sweep(&r);
    ASSERT_CONTEXT(2 == gone, "expected 2 removals, got %lu", (unsigned long)gone);
    ASSERT_CONTEXT(!File__exists(a), "transient %s survived", a);
    ASSERT_CONTEXT(!File__exists(c), "transient %s survived", c);
    ASSERT_CONTEXT(File__exists(b), "persisted %s was deleted", b);
    (void)File__unlink(b);
  }

  // ---
  // Scenario: re-adding a path updates it instead of duplicating it,
  // and persist is sticky -- a later transient write must not revoke a keep
  {
    Registry__init(&r);
    touch(a);
    ASSERT(Registry__add(&r, a, false));
    ASSERT(Registry__add(&r, a, true));
    ASSERT_CONTEXT(1 == r.ct, "duplicate entry created (ct=%lu)", (unsigned long)r.ct);
    ASSERT(Registry__add(&r, a, false));
    ASSERT_CONTEXT(1 == r.ct, "duplicate entry created (ct=%lu)", (unsigned long)r.ct);

    (void)Registry__sweep(&r);
    ASSERT_CONTEXT(File__exists(a), "persist was revoked by a later transient write");
    (void)File__unlink(a);
  }

  // ---
  // Scenario: a second sweep is harmless
  {
    Registry__init(&r);
    touch(a);
    ASSERT(Registry__add(&r, a, false));
    ASSERT(1 == Registry__sweep(&r));
    ASSERT(0 == Registry__sweep(&r));  // the path is already gone
    ASSERT(!File__exists(a));
  }

  // ---
  // Scenario: the table fills up and says so rather than silently losing files
  {
    Registry__init(&r);
    char p[MAX_PATH];
    for (u32 i = 0; i < MAX_TRACKED_FILES; i++) {
      (void)snprintf(p, sizeof(p), "%s/bulk_%lu.jpg", DIR, (unsigned long)i);
      ASSERT_CONTEXT(Registry__add(&r, p, true), "add %lu failed early", (unsigned long)i);
    }
    (void)snprintf(p, sizeof(p), "%s/one_too_many.jpg", DIR);
    ASSERT_CONTEXT(!Registry__add(&r, p, false), "the full table accepted another entry");
    ASSERT_CONTEXT(1 == r.dropped, "dropped counter not incremented");
    ASSERT(MAX_TRACKED_FILES == r.ct);
  }

  // ---
  // Scenario: sweeping a path that no longer exists is not an error
  {
    Registry__init(&r);
    char missing[MAX_PATH];
    (void)snprintf(missing, sizeof(missing), "%s/never_written.jpg", DIR);
    ASSERT(Registry__add(&r, missing, false));
    ASSERT_CONTEXT(1 == Registry__sweep(&r), "an absent file should count as removed");
  }

  Console__out("Registry: all scenarios passed\n");
  return 0;
}
