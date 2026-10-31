#define CAMERA_TEST
#include "../src/unity.h"  // IWYU pragma: keep

// A bare IPC client for exercising camera_preview without an MCP session.
// Sends one request to a running preview daemon and prints its reply.
//
//   ipc_client <camera> ping
//   ipc_client <camera> grab [file] [persist] [quality]
//   ipc_client <camera> die

int main(int argc, char* argv[]) {
  Console__init();
  if (argc < 3) {
    Console__error("usage: ipc_client <camera> ping|grab|die [file] [persist] [quality]\n");
    return 2;
  }

  CameraList cams;
  Camera cam;
  if (!CamId__list(&cams) || !CamId__resolve(&cams, argv[1], &cam)) {
    Console__error("no camera matches '%s'\n", argv[1]);
    return 1;
  }

  IpcReq req = {.quality = 90};
  if (cstr__eq(argv[2], "ping")) {
    req.kind = IPC_REQ_PING;
  } else if (cstr__eq(argv[2], "grab")) {
    req.kind = IPC_REQ_GRAB;
    if (argc > 3) {
      (void)cstr__copy(req.file, (u32)sizeof(req.file), argv[3]);
    }
    if (argc > 4) {
      req.persist = cstr__eq(argv[4], "true") || cstr__eq(argv[4], "1");
    }
    if (argc > 5) {
      (void)cstr__toU32(argv[5], &req.quality);
    }
  } else if (cstr__eq(argv[2], "die")) {
    req.kind = IPC_REQ_DIE;
  } else {
    Console__error("unknown command '%s'\n", argv[2]);
    return 2;
  }

  char sock[MAX_PATH];
  if (!Ipc__sockPath(sock, (u32)sizeof(sock), &cam)) {
    return 1;
  }
  int fd = -1;
  if (!Ipc__connect(sock, &fd)) {
    Console__error("no preview listening on %s\n", sock);
    return 1;
  }

  char line[MAX_JSON_BYTES];
  if (!Ipc__reqEncode(&req, line, (u32)sizeof(line)) || !Ipc__writeLine(fd, line)) {
    return 1;
  }
  u32 len = 0;
  if (!Ipc__readLine(fd, line, (u32)sizeof(line), 20000, &len)) {
    Console__error("no reply\n");
    return 1;
  }
  Console__out("%s\n", line);
  (void)close(fd);
  return 0;
}
