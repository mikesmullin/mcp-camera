#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// @describe CamId
// @tag app
//
// Naming and id resolution are tested against a synthetic camera list so the
// results do not depend on what hardware is plugged into the build machine.
// The live enumeration is covered at the end, but only as a smoke check.

// build a camera the way _CamId__probe() would, then derive its slug
static Camera mk(u32 index, const char* path, const char* card, const char* serial) {
  Camera c;
  memset(&c, 0, sizeof(c));
  c.index = index;
  (void)cstr__copy(c.path, (u32)sizeof(c.path), path);
  (void)cstr__copy(c.card, (u32)sizeof(c.card), card);
  (void)cstr__copy(c.serial, (u32)sizeof(c.serial), serial);
  CamId__nameOf(&c);
  return c;
}

int main(void) {
  Console__init();

  // ---
  // Scenario: the id is a fully lowercased, underscored slug
  {
    Camera c = mk(0, "/dev/video0", "UVC Camera (046d:0821)", "6A7559A0");
    ASSERT_CONTEXT(cstr__eq(c.name, "0__uvc_camera_046d_0821__6a7559a0"), "got '%s'", c.name);

    // the separator between components stays a double underscore even though
    // each component collapses its own separator runs
    Camera d = mk(11, "/dev/video11", "Integrated   RGB  Camera", "AB-12:34");
    ASSERT_CONTEXT(cstr__eq(d.name, "11__integrated_rgb_camera__ab_12_34"), "got '%s'", d.name);

    // a device with no serial is still addressable
    Camera e = mk(2, "/dev/video2", "Virtual Cam", "");
    ASSERT_CONTEXT(cstr__eq(e.name, "2__virtual_cam__unknown"), "got '%s'", e.name);

    // a card name made entirely of punctuation cannot produce an empty field
    Camera f = mk(3, "/dev/video3", "***", "");
    ASSERT_CONTEXT(cstr__eq(f.name, "3__unknown__unknown"), "got '%s'", f.name);
  }

  // ---
  // Scenario: resolution accepts every documented form
  {
    CameraList list;
    memset(&list, 0, sizeof(list));
    list.items[0] = mk(0, "/dev/video0", "UVC Camera (046d:0821)", "6A7559A0");
    list.items[1] = mk(2, "/dev/video2", "Logitech BRIO", "ZZ99");
    list.ct = 2;

    Camera got;
    // empty and "default" pick the first camera
    ASSERT(CamId__resolve(&list, "", &got) && 0 == got.index);
    ASSERT(CamId__resolve(&list, "default", &got) && 0 == got.index);
    ASSERT(CamId__resolve(&list, NULL, &got) && 0 == got.index);

    // node index
    ASSERT(CamId__resolve(&list, "2", &got) && 2 == got.index);
    // device path
    ASSERT(CamId__resolve(&list, "/dev/video2", &got) && 2 == got.index);
    // the full slug
    ASSERT(CamId__resolve(&list, "2__logitech_brio__zz99", &got) && 2 == got.index);
    // a slug substring
    ASSERT(CamId__resolve(&list, "brio", &got) && 2 == got.index);
    // the human-readable card name, which is NOT a substring of the slug
    ASSERT_CONTEXT(
        CamId__resolve(&list, "Logitech BRIO", &got) && 2 == got.index,
        "the raw card name should still resolve");
    // the serial in its original case
    ASSERT(CamId__resolve(&list, "6A7559A0", &got) && 0 == got.index);
  }

  // ---
  // Scenario: a numeric id that matches no node fails instead of falling
  // through to a substring match on some unrelated camera
  {
    CameraList list;
    memset(&list, 0, sizeof(list));
    list.items[0] = mk(0, "/dev/video0", "Cam 7", "7");
    list.ct = 1;

    Camera got;
    ASSERT_CONTEXT(
        !CamId__resolve(&list, "7", &got),
        "index 7 does not exist and must not match 'Cam 7' by substring");
    ASSERT(CamId__resolve(&list, "0", &got) && 0 == got.index);
  }

  // ---
  // Scenario: nothing resolves against an empty list
  {
    CameraList list;
    memset(&list, 0, sizeof(list));
    Camera got;
    ASSERT(!CamId__resolve(&list, "", &got));
    ASSERT(!CamId__resolve(&list, "0", &got));
  }

  // ---
  // Scenario: live enumeration is self-consistent (skipped with no hardware)
  {
    CameraList live;
    if (CamId__list(&live) && live.ct > 0) {
      for (u32 i = 0; i < live.ct; i++) {
        Camera* c = &live.items[i];
        ASSERT_CONTEXT(0 != c->name[0], "camera %lu has no name", (unsigned long)i);
        ASSERT_CONTEXT(0 != c->path[0], "camera %lu has no device path", (unsigned long)i);
        // every enumerated camera must resolve by its own reported id
        Camera got;
        ASSERT_CONTEXT(
            CamId__resolve(&live, c->name, &got) && got.index == c->index,
            "'%s' does not resolve to itself",
            c->name);
        // the slug must contain only the characters we promise
        for (u32 k = 0; 0 != c->name[k]; k++) {
          char ch = c->name[k];
          bool ok = ('a' <= ch && ch <= 'z') || ('0' <= ch && ch <= '9') || '_' == ch;
          ASSERT_CONTEXT(ok, "'%s' contains the illegal character '%c'", c->name, ch);
        }
        Console__out("   live: %s -> %s\n", c->name, c->path);
      }
    } else {
      Console__out("   live: no cameras present; enumeration check skipped\n");
    }
  }

  Console__out("CamId: all scenarios passed\n");
  return 0;
}
