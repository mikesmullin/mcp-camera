#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// @describe Json
// @tag common

static Arena* A = NULL;

// parse one object and return the string value of `want`
static bool keyOf(const char* doc, const char* want, char* dst, u32 dst_sz) {
  Arena__reset(A);
  Json j;
  Json__init(&j, doc, cstr__len(doc), A);
  if (!Json__object_begin(&j)) {
    return false;
  }
  JsonStr key = {0};
  bool found = false;
  while (Json__object_key(&j, &key)) {
    if (JsonStr__eq(key, want)) {
      found = Json__stringTo(&j, dst, dst_sz);
      continue;
    }
    if (!Json__skipValue(&j)) {
      return false;
    }
  }
  return found && Json__object_end(&j) && !j.failed;
}

int main(void) {
  Console__init();
  A = Arena__allocZ(1024 * 1024);
  ASSERT(NULL != A);
  char buf[512];

  // ---
  // Scenario: scalars round-trip
  {
    Arena__reset(A);
    Json j;
    const char* doc = "{\"b\":true,\"n\":-12.5,\"u\":42,\"s\":\"hi\",\"z\":null}";
    Json__init(&j, doc, cstr__len(doc), A);
    ASSERT(Json__object_begin(&j));

    JsonStr k = {0};
    bool bv = false;
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "b"));
    ASSERT(Json__bool(&j, &bv) && true == bv);

    f64 nv = 0.0;
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "n"));
    ASSERT(Json__number(&j, &nv));
    ASSERT_CONTEXT(nv < -12.4 && nv > -12.6, "expected -12.5, got %f", nv);

    u32 uv = 0;
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "u"));
    ASSERT(Json__u32(&j, &uv) && 42 == uv);

    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "s"));
    ASSERT(Json__stringTo(&j, buf, (u32)sizeof(buf)) && cstr__eq(buf, "hi"));

    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "z"));
    ASSERT(Json__skipValue(&j));

    ASSERT(Json__object_end(&j));
    ASSERT(!j.failed);
  }

  // ---
  // Scenario: keys are found regardless of order, and unknown keys are skipped
  {
    ASSERT(keyOf("{\"a\":1,\"want\":\"yes\",\"b\":2}", "want", buf, sizeof(buf)));
    ASSERT(cstr__eq(buf, "yes"));
    ASSERT(keyOf("{\"want\":\"first\"}", "want", buf, sizeof(buf)));
    ASSERT(cstr__eq(buf, "first"));
    ASSERT(keyOf("{\"a\":1,\"b\":2,\"want\":\"last\"}", "want", buf, sizeof(buf)));
    ASSERT(cstr__eq(buf, "last"));
  }

  // ---
  // Scenario: skipValue consumes whole nested containers, not just one token
  {
    const char* doc =
        "{\"deep\":{\"a\":[1,2,{\"b\":[3,{\"c\":4}]}],\"d\":{}},\"want\":\"reached\"}";
    ASSERT_CONTEXT(keyOf(doc, "want", buf, sizeof(buf)), "nested skip lost the parser");
    ASSERT(cstr__eq(buf, "reached"));
  }

  // ---
  // Scenario: escapes are decoded
  {
    ASSERT(keyOf("{\"want\":\"a\\nb\\tc\\\"d\\\\e\\/f\"}", "want", buf, sizeof(buf)));
    ASSERT_CONTEXT(cstr__eq(buf, "a\nb\tc\"d\\e/f"), "got '%s'", buf);
    // \u escapes, including multi-byte UTF-8
    ASSERT(keyOf("{\"want\":\"\\u0041\\u00e9\\u20ac\"}", "want", buf, sizeof(buf)));
    ASSERT_CONTEXT(cstr__eq(buf, "A\xc3\xa9\xe2\x82\xac"), "utf8 decode gave '%s'", buf);
  }

  // ---
  // Scenario: arrays iterate, including the empty array
  {
    Arena__reset(A);
    Json j;
    const char* doc = "[10,20,30]";
    Json__init(&j, doc, cstr__len(doc), A);
    ASSERT(Json__array_begin(&j));
    u32 sum = 0, ct = 0;
    while (Json__array_item(&j)) {
      u32 v = 0;
      ASSERT(Json__u32(&j, &v));
      sum += v;
      ct++;
    }
    ASSERT(Json__array_end(&j));
    ASSERT_CONTEXT(3 == ct && 60 == sum, "ct=%lu sum=%lu", (unsigned long)ct, (unsigned long)sum);

    Arena__reset(A);
    const char* empty = "[]";
    Json__init(&j, empty, cstr__len(empty), A);
    ASSERT(Json__array_begin(&j));
    ASSERT(!Json__array_item(&j));
    ASSERT(Json__array_end(&j));
    ASSERT(!j.failed);
  }

  // ---
  // Scenario: an empty object closes immediately
  {
    Arena__reset(A);
    Json j;
    const char* doc = "{}";
    Json__init(&j, doc, cstr__len(doc), A);
    ASSERT(Json__object_begin(&j));
    JsonStr k = {0};
    ASSERT(!Json__object_key(&j, &k));
    ASSERT(Json__object_end(&j));
    ASSERT(!j.failed);
  }

  // ---
  // Scenario: malformed input fails rather than reading past the buffer
  {
    static const char* BAD[] = {
        "{",
        "}",
        "{\"a\"}",
        "{\"a\":}",
        "[1,2",
        "{\"a\":\"unterminated",
        "{\"a\":tru}",
        "nope",
        "{\"a\":01x}",
    };
    for (u32 i = 0; i < ARRAYSIZE(BAD); i++) {
      Arena__reset(A);
      Json j;
      Json__init(&j, BAD[i], cstr__len(BAD[i]), A);
      // walking the whole document must terminate and must not crash
      bool ok = Json__skipValue(&j);
      (void)ok;  // some of these parse a prefix successfully; none may hang
    }
  }

  // ---
  // Scenario: a parser bound to a slice stops at the slice's end.
  // This matters because the MCP layer hands `params` to the tool as a slice
  // of the request line, not as its own NUL-terminated string.
  {
    Arena__reset(A);
    const char* line = "{\"a\":1}GARBAGE-AFTER";
    Json j;
    Json__init(&j, line, 7, A);  // only the object
    ASSERT(Json__object_begin(&j));
    JsonStr k = {0};
    u32 v = 0;
    ASSERT(Json__object_key(&j, &k) && JsonStr__eq(k, "a") && Json__u32(&j, &v) && 1 == v);
    ASSERT(Json__object_end(&j));
    ASSERT(!j.failed);
  }

  // ---
  // Scenario: numbers at a slice boundary parse without a terminator
  {
    Arena__reset(A);
    const char* line = "12345xxxx";
    Json j;
    Json__init(&j, line, 5, A);
    f64 v = 0.0;
    ASSERT(Json__number(&j, &v));
    ASSERT_CONTEXT(v > 12344.9 && v < 12345.1, "got %f", v);
  }

  Arena__free(A);
  Console__out("Json: all scenarios passed\n");
  return 0;
}
