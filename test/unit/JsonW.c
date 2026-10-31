#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// @describe JsonW
// @tag common

int main(void) {
  Console__init();
  Arena* A = Arena__allocZ(1024 * 1024);
  ASSERT(NULL != A);
  char buf[4096];

  // ---
  // Scenario: a nested document serializes with correct comma placement
  {
    JsonW w;
    JsonW__init(&w, buf, (u32)sizeof(buf));
    JsonW__objBegin(&w);
    JsonW__kstr(&w, "a", "one");
    JsonW__ku32(&w, "b", 2);
    JsonW__kbool(&w, "c", true);
    JsonW__key(&w, "d");
    JsonW__arrBegin(&w);
    JsonW__u32(&w, 1);
    JsonW__u32(&w, 2);
    JsonW__objBegin(&w);
    JsonW__kstr(&w, "e", "f");
    JsonW__objEnd(&w);
    JsonW__arrEnd(&w);
    JsonW__key(&w, "g");
    JsonW__objBegin(&w);
    JsonW__objEnd(&w);
    JsonW__objEnd(&w);
    ASSERT(JsonW__ok(&w));
    ASSERT_CONTEXT(
        cstr__eq(buf, "{\"a\":\"one\",\"b\":2,\"c\":true,\"d\":[1,2,{\"e\":\"f\"}],\"g\":{}}"),
        "got %s",
        buf);
  }

  // ---
  // Scenario: everything written is re-readable by the parser
  {
    JsonW w;
    JsonW__init(&w, buf, (u32)sizeof(buf));
    JsonW__objBegin(&w);
    JsonW__kstr(&w, "text", "line1\nline2\t\"quoted\" \\slash\\ \x01ctrl");
    JsonW__ks64(&w, "neg", -1234567890LL);
    JsonW__kf64(&w, "score", 0.98765, 4);
    JsonW__objEnd(&w);
    ASSERT(JsonW__ok(&w));

    Arena__reset(A);
    Json j;
    Json__init(&j, buf, w.len, A);
    ASSERT(Json__object_begin(&j));
    JsonStr k = {0};
    char got[512];
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "text"));
    ASSERT(Json__stringTo(&j, got, (u32)sizeof(got)));
    ASSERT_CONTEXT(
        cstr__eq(got, "line1\nline2\t\"quoted\" \\slash\\ \x01ctrl"),
        "round trip lost data: '%s'",
        got);
    f64 n = 0.0;
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "neg") && Json__number(&j, &n));
    ASSERT_CONTEXT(n < -1234567889.0 && n > -1234567891.0, "got %f", n);
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "score") && Json__number(&j, &n));
    // 0.98765 written with 4 decimals rounds to 0.9877
    ASSERT_CONTEXT(n > 0.98765 && n < 0.98775, "got %f", n);
    ASSERT(Json__object_end(&j) && !j.failed);
  }

  // ---
  // Scenario: a streamed string matches one written in a single call.
  // This is the path the base64 data url takes.
  {
    char a[256], b[256];
    JsonW w1;
    JsonW__init(&w1, a, (u32)sizeof(a));
    JsonW__str(&w1, "data:image/jpeg;base64,AAAA+/==");
    ASSERT(JsonW__ok(&w1));

    JsonW w2;
    JsonW__init(&w2, b, (u32)sizeof(b));
    JsonW__strOpen(&w2);
    JsonW__strPart(&w2, "data:image/jpeg;base64,", 23);
    JsonW__strPart(&w2, "AAAA+/==", 8);
    JsonW__strClose(&w2);
    ASSERT(JsonW__ok(&w2));
    ASSERT_CONTEXT(cstr__eq(a, b), "streamed '%s' != single '%s'", b, a);
  }

  // ---
  // Scenario: overflow is sticky and never writes past the buffer
  {
    char small[24];
    // poison the byte just past the buffer to catch an off-by-one
    char guard[32];
    memset(guard, 0x7E, sizeof(guard));
    JsonW w;
    JsonW__init(&w, guard, (u32)sizeof(small));
    JsonW__objBegin(&w);
    for (u32 i = 0; i < 50; i++) {
      JsonW__kstr(&w, "a_long_key_name", "a_long_value_string");
    }
    JsonW__objEnd(&w);
    ASSERT_CONTEXT(!JsonW__ok(&w), "overflow was not reported");
    for (u32 i = sizeof(small); i < sizeof(guard); i++) {
      ASSERT_CONTEXT(0x7E == (u8)guard[i], "wrote past the buffer at +%lu", (unsigned long)i);
    }
  }

  // ---
  // Scenario: raw splices pre-serialized JSON in verbatim
  {
    JsonW w;
    JsonW__init(&w, buf, (u32)sizeof(buf));
    JsonW__objBegin(&w);
    JsonW__kraw(&w, "regions", "[{\"x\":1},{\"x\":2}]");
    JsonW__ku32(&w, "after", 7);
    JsonW__objEnd(&w);
    ASSERT(JsonW__ok(&w));
    ASSERT_CONTEXT(cstr__eq(buf, "{\"regions\":[{\"x\":1},{\"x\":2}],\"after\":7}"), "got %s", buf);
  }

  // ---
  // Scenario: a NULL string value writes an empty string, not a crash
  {
    JsonW w;
    JsonW__init(&w, buf, (u32)sizeof(buf));
    JsonW__objBegin(&w);
    JsonW__kstr(&w, "k", NULL);
    JsonW__objEnd(&w);
    ASSERT(JsonW__ok(&w));
    ASSERT_CONTEXT(cstr__eq(buf, "{\"k\":\"\"}"), "got %s", buf);
  }

  Arena__free(A);
  Console__out("JsonW: all scenarios passed\n");
  return 0;
}
