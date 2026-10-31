#define CAMERA_TEST
#include "../../src/unity.h"  // IWYU pragma: keep

// @describe Ipc
// @tag app
//
// The wire format between camera_mcp and camera_preview. These are pure
// encode/decode checks; the socket itself is exercised by the integration run.

int main(void) {
  Console__init();
  Arena* A = Arena__allocZ(1024 * 1024);
  ASSERT(NULL != A);
  char line[MAX_JSON_BYTES];

  // ---
  // Scenario: a request round-trips with every field intact
  {
    IpcReq in = {.kind = IPC_REQ_GRAB, .persist = true, .quality = 73};
    ASSERT(cstr__copy(in.file, (u32)sizeof(in.file), "/tmp/camera/a b/\"odd\"\\name.jpg"));
    ASSERT(Ipc__reqEncode(&in, line, (u32)sizeof(line)));

    Arena__reset(A);
    IpcReq out;
    ASSERT_CONTEXT(Ipc__reqDecode(line, cstr__len(line), A, &out), "decode failed on %s", line);
    ASSERT(IPC_REQ_GRAB == out.kind);
    ASSERT_CONTEXT(cstr__eq(out.file, in.file), "path round trip: '%s'", out.file);
    ASSERT(true == out.persist);
    ASSERT_CONTEXT(73 == out.quality, "quality %lu", (unsigned long)out.quality);
  }

  // ---
  // Scenario: every request kind survives the round trip
  {
    IpcReqKind kinds[] = {IPC_REQ_PING, IPC_REQ_GRAB, IPC_REQ_DIE};
    for (u32 i = 0; i < ARRAYSIZE(kinds); i++) {
      IpcReq in = {.kind = kinds[i], .quality = 90};
      ASSERT(Ipc__reqEncode(&in, line, (u32)sizeof(line)));
      Arena__reset(A);
      IpcReq out;
      ASSERT(Ipc__reqDecode(line, cstr__len(line), A, &out));
      ASSERT_CONTEXT(kinds[i] == out.kind, "kind %d did not survive", (int)kinds[i]);
    }
  }

  // ---
  // Scenario: a response round-trips, including an error string
  {
    IpcRes in = {
        .ok = false,
        .width = 640,
        .height = 480,
        .bytes = 66036,
        .captured_at = 1793474271320LL,
        .persist = true};
    ASSERT(cstr__copy(in.path, (u32)sizeof(in.path), "/tmp/camera/20261031/x.jpg"));
    ASSERT(cstr__copy(in.error, (u32)sizeof(in.error), "cannot write /tmp/x: no space"));
    ASSERT(Ipc__resEncode(&in, line, (u32)sizeof(line)));

    Arena__reset(A);
    IpcRes out;
    ASSERT(Ipc__resDecode(line, cstr__len(line), A, &out));
    ASSERT(false == out.ok);
    ASSERT(cstr__eq(out.path, in.path));
    ASSERT(640 == out.width && 480 == out.height && 66036 == out.bytes);
    ASSERT_CONTEXT(
        1793474271320LL == out.captured_at,
        "timestamp lost precision: %lld",
        (long long)out.captured_at);
    ASSERT(true == out.persist);
    ASSERT(cstr__eq(out.error, in.error));
  }

  // ---
  // Scenario: unknown fields are ignored, so either side can add one
  {
    const char* doc =
        "{\"t\":\"grab\",\"future\":{\"nested\":[1,2]},\"file\":\"/x.jpg\",\"extra\":true}";
    Arena__reset(A);
    IpcReq out;
    ASSERT_CONTEXT(Ipc__reqDecode(doc, cstr__len(doc), A, &out), "forward compatibility broke");
    ASSERT(IPC_REQ_GRAB == out.kind);
    ASSERT(cstr__eq(out.file, "/x.jpg"));
  }

  // ---
  // Scenario: defaults are applied for omitted fields
  {
    const char* doc = "{\"t\":\"grab\"}";
    Arena__reset(A);
    IpcReq out;
    ASSERT(Ipc__reqDecode(doc, cstr__len(doc), A, &out));
    ASSERT(cstr__eq(out.file, ""));
    ASSERT(false == out.persist);
    ASSERT_CONTEXT(
        90 == out.quality,
        "default quality should be 90, got %lu",
        (unsigned long)out.quality);
  }

  // ---
  // Scenario: a nonsense quality is clamped back to the default
  {
    const char* docs[] = {"{\"t\":\"grab\",\"quality\":0}", "{\"t\":\"grab\",\"quality\":9999}"};
    for (u32 i = 0; i < ARRAYSIZE(docs); i++) {
      Arena__reset(A);
      IpcReq out;
      ASSERT(Ipc__reqDecode(docs[i], cstr__len(docs[i]), A, &out));
      ASSERT_CONTEXT(90 == out.quality, "quality not clamped: %lu", (unsigned long)out.quality);
    }
  }

  // ---
  // Scenario: malformed or unknown requests are rejected
  {
    const char* BAD[] = {
        "",
        "{}",
        "not json",
        "{\"t\":\"explode\"}",
        "{\"t\":42}",
        "{\"t\":\"grab\"",
    };
    for (u32 i = 0; i < ARRAYSIZE(BAD); i++) {
      Arena__reset(A);
      IpcReq out;
      ASSERT_CONTEXT(
          !Ipc__reqDecode(BAD[i], cstr__len(BAD[i]), A, &out),
          "accepted bad request '%s'",
          BAD[i]);
    }
  }

  // ---
  // Scenario: a path too long for the buffer fails encoding rather than
  // silently producing a truncated path the daemon would then write to
  {
    IpcReq in = {.kind = IPC_REQ_GRAB, .quality = 90};
    for (u32 i = 0; i < MAX_PATH - 1; i++) {
      in.file[i] = 'x';
    }
    in.file[MAX_PATH - 1] = 0;
    ASSERT(Ipc__reqEncode(&in, line, (u32)sizeof(line)));
    Arena__reset(A);
    IpcReq out;
    ASSERT(Ipc__reqDecode(line, cstr__len(line), A, &out));
    ASSERT_CONTEXT(cstr__eq(out.file, in.file), "a max-length path must survive");

    // and a tiny output buffer must report overflow
    char tiny[16];
    ASSERT(!Ipc__reqEncode(&in, tiny, (u32)sizeof(tiny)));
  }

  Arena__free(A);
  Console__out("Ipc: all scenarios passed\n");
  return 0;
}
