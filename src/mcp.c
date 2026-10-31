// Single Compilation Unit 1 (Unity Build: camera_mcp) ---------
//
// The MCP stdio server. It owns no hardware: every frame comes from a
// camera_preview daemon over a Unix socket (see app/Preview.c for why).

#include "unity.h"  // IWYU pragma: keep

// clang-format off
#include "app/Http.c"  // IWYU pragma: keep
#include "app/Vision.c"  // IWYU pragma: keep
#include "app/Preview.c"  // IWYU pragma: keep
#include "app/Mcp.c"  // IWYU pragma: keep
// clang-format on

#define MCP_ARENA_BYTES (48ULL * 1024ULL * 1024ULL)

// process-wide state
typedef struct {
  Arena* arena;  // per-request scratch, reset between requests
} Mcp__State;

Mcp__State* _G = &(Mcp__State){0};

// stdin closing is the normal shutdown path; signals are the abnormal one
static void _Main__onSignal(int sig) {
  Console__error("[camera] caught signal %d, exiting\n", sig);
  _exit(0);
}

// print the cameras this machine exposes, for `camera_mcp --list`
static int _Main__listCameras(void) {
  CameraList cams;
  if (!CamId__list(&cams)) {
    return 1;
  }
  if (0 == cams.ct) {
    Console__error("no capture-capable cameras found\n");
    return 1;
  }
  for (u32 i = 0; i < cams.ct; i++) {
    Console__out("%s\t%s\n", cams.items[i].name, cams.items[i].path);
  }
  return 0;
}

int main(int argc, char* argv[]) {
  Console__init();
  Jpeg__init();

  (void)signal(SIGINT, _Main__onSignal);
  (void)signal(SIGTERM, _Main__onSignal);
  // the preview daemon is detached; never leave a zombie behind for it
  (void)signal(SIGCHLD, SIG_IGN);
  // a client that closes the transport mid-write must not kill us
  (void)signal(SIGPIPE, SIG_IGN);

  for (int i = 1; i < argc; i++) {
    if (cstr__eq(argv[i], "--list")) {
      return _Main__listCameras();
    }
    if (cstr__eq(argv[i], "--version")) {
      Console__out("%s %s\n", MCP_SERVER_NAME, MCP_SERVER_VERSION);
      return 0;
    }
    if (cstr__eq(argv[i], "--help")) {
      Console__out(
          "usage: camera_mcp [--list|--version|--help]\n"
          "\n"
          "With no arguments, speaks MCP (JSON-RPC 2.0) over stdin/stdout and\n"
          "exposes one tool: camera_look.\n"
          "\n"
          "environment:\n"
          "  CAMERA_DIR          transient frame directory (default /tmp/camera)\n"
          "  CAMERA_OCR_URL      OCR backend      (default http://127.0.0.1:1239)\n"
          "  CAMERA_VLM_URL      VLM backend      (default http://127.0.0.1:1246)\n"
          "  CAMERA_PREVIEW_BIN  camera_preview path (default: beside this binary)\n");
      return 0;
    }
    Console__error("unknown argument: %s (try --help)\n", argv[i]);
    return 2;
  }

  _G->arena = Arena__alloc(MCP_ARENA_BYTES);
  ASSERT_CONTEXT(
      _G->arena,
      "failed to allocate the %llu byte request arena",
      (unsigned long long)MCP_ARENA_BYTES);

  Mcp__serve(_G->arena);

  Arena__free(_G->arena);
  return 0;
}
