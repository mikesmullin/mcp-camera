#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// @describe String
// @tag common

int main(void) {
  Console__init();
  char buf[64];

  // ---
  // Scenario: copy always terminates, and reports truncation
  {
    ASSERT(cstr__copy(buf, (u32)sizeof(buf), "hello"));
    ASSERT(cstr__eq(buf, "hello"));

    char small[4];
    ASSERT_CONTEXT(!cstr__copy(small, (u32)sizeof(small), "hello"), "truncation not reported");
    ASSERT_CONTEXT(cstr__eq(small, "hel"), "got '%s'", small);
    ASSERT(0 == small[3]);

    // a one-byte buffer can hold only the terminator
    char one[1];
    ASSERT(!cstr__copy(one, 1, "x"));
    ASSERT(0 == one[0]);
    // a zero-size buffer must be refused, not written to
    ASSERT(!cstr__copy(buf, 0, "x"));
  }

  // ---
  // Scenario: cat appends and respects the bound
  {
    (void)cstr__copy(buf, (u32)sizeof(buf), "a/");
    ASSERT(cstr__cat(buf, (u32)sizeof(buf), "b"));
    ASSERT(cstr__eq(buf, "a/b"));

    char small[6];
    (void)cstr__copy(small, (u32)sizeof(small), "abcd");
    ASSERT(!cstr__cat(small, (u32)sizeof(small), "efgh"));
    ASSERT_CONTEXT(cstr__eq(small, "abcde"), "got '%s'", small);
  }

  // ---
  // Scenario: comparisons
  {
    ASSERT(cstr__eq("abc", "abc"));
    ASSERT(!cstr__eq("abc", "abcd"));
    ASSERT(!cstr__eq("abcd", "abc"));
    ASSERT(!cstr__eq("abc", "abd"));
    ASSERT(cstr__eq("", ""));
    ASSERT(!cstr__eq(NULL, "a"));

    ASSERT(cstr__ieqn(3, "ABC", "abc"));
    ASSERT(!cstr__ieqn(3, "ABC", "abd"));

    ASSERT(cstr__contains("hello world", "o w"));
    ASSERT(!cstr__contains("hello", "xyz"));
    ASSERT_CONTEXT(cstr__contains("hello", ""), "an empty needle must match");
    ASSERT(cstr__icontains("COUNTY CLERK", "county"));
    ASSERT(cstr__icontains("county clerk", "CLERK"));
    ASSERT(!cstr__icontains("abc", "abcd"));

    ASSERT(cstr__endsWith("frame.jpg", ".jpg"));
    ASSERT(!cstr__endsWith("frame.jpg", ".png"));
    ASSERT(cstr__endsWith("x", "x"));
    ASSERT(!cstr__endsWith("x", "xx"));
  }

  // ---
  // Scenario: strict unsigned parsing
  {
    u32 v = 0;
    ASSERT(cstr__toU32("0", &v) && 0 == v);
    ASSERT(cstr__toU32("42", &v) && 42 == v);
    ASSERT(cstr__toU32("4294967295", &v) && 4294967295u == v);
    ASSERT_CONTEXT(!cstr__toU32("4294967296", &v), "overflow must be refused");
    ASSERT_CONTEXT(!cstr__toU32("-1", &v), "a sign must be refused");
    ASSERT_CONTEXT(!cstr__toU32("12a", &v), "trailing junk must be refused");
    ASSERT_CONTEXT(!cstr__toU32("", &v), "an empty string must be refused");
    ASSERT(!cstr__toU32(" 1", &v));
  }

  // ---
  // Scenario: trailing whitespace is trimmed in place
  {
    (void)cstr__copy(buf, (u32)sizeof(buf), "UVC Camera  \n\r\t");
    cstr__trimRight(buf);
    ASSERT_CONTEXT(cstr__eq(buf, "UVC Camera"), "got '%s'", buf);
    (void)cstr__copy(buf, (u32)sizeof(buf), "   ");
    cstr__trimRight(buf);
    ASSERT(cstr__eq(buf, ""));
  }

  // ---
  // Scenario: slugify produces the lowercase ids camera_look reports
  {
    ASSERT(cstr__slugify(buf, (u32)sizeof(buf), "UVC Camera (046d:0821)"));
    ASSERT_CONTEXT(cstr__eq(buf, "uvc_camera_046d_0821"), "got '%s'", buf);

    ASSERT(cstr__slugify(buf, (u32)sizeof(buf), "6A7559A0"));
    ASSERT_CONTEXT(cstr__eq(buf, "6a7559a0"), "got '%s'", buf);

    // runs of separators collapse, and no leading or trailing underscore
    ASSERT(cstr__slugify(buf, (u32)sizeof(buf), "  --Foo // Bar!!  "));
    ASSERT_CONTEXT(cstr__eq(buf, "foo_bar"), "got '%s'", buf);

    // nothing usable means failure, with an empty result
    ASSERT(!cstr__slugify(buf, (u32)sizeof(buf), "!!!"));
    ASSERT(cstr__eq(buf, ""));
    ASSERT(!cstr__slugify(buf, (u32)sizeof(buf), ""));

    // truncation must not leave a dangling separator or overrun
    char tiny[8];
    ASSERT(cstr__slugify(tiny, (u32)sizeof(tiny), "abcdefghijklmnop"));
    ASSERT_CONTEXT(cstr__len(tiny) <= 7, "slug overran: '%s'", tiny);
  }

  // ---
  // Scenario: base64 matches known vectors (RFC 4648)
  {
    char out[64];
    struct {
      const char* in;
      const char* want;
    } V[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (u32 i = 0; i < ARRAYSIZE(V); i++) {
      u32 n = cstr__len(V[i].in);
      ASSERT(Base64__encode(out, (u64)sizeof(out), (const u8*)V[i].in, n));
      ASSERT_CONTEXT(
          cstr__eq(out, V[i].want),
          "base64('%s') = '%s', want '%s'",
          V[i].in,
          out,
          V[i].want);
      ASSERT_CONTEXT(
          Base64__encodedLen(n) == (u64)cstr__len(out) + 1ULL,
          "encodedLen(%lu) disagrees with the actual %lu",
          (unsigned long)n,
          (unsigned long)cstr__len(out));
    }

    // all byte values survive, and a too-small buffer is refused
    u8 raw[256];
    for (u32 i = 0; i < 256; i++) {
      raw[i] = (u8)i;
    }
    char big[Base64__encodedLen(256)];
    ASSERT(Base64__encode(big, sizeof(big), raw, 256));
    ASSERT_CONTEXT(
        344 == cstr__len(big),
        "expected 344 chars, got %lu",
        (unsigned long)cstr__len(big));
    ASSERT(!Base64__encode(big, 10, raw, 256));
  }

  Console__out("String: all scenarios passed\n");
  return 0;
}
