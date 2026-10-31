#pragma once

#include <sys/wait.h>  // IWYU pragma: keep // waitpid()
#include <unistd.h>  // IWYU pragma: keep // fork(), execv(), readlink()

#include "../unity.h"  // IWYU pragma: keep

// The camera_mcp side of the preview daemon's lifecycle.
//
// camera_preview owns the camera exclusively -- a V4L2 device cannot be opened
// for streaming twice -- so camera_mcp never touches /dev/videoN itself. Every
// frame comes from the daemon, which guarantees the JPEG the agent analyses is
// byte-identical to the frame the user just saw freeze on screen.
//
// The daemon is deliberately detached: it outlives this MCP session and retires
// on its own idle timer, so a frame is never lost to a client reconnect.

#define PREVIEW_START_TIMEOUT_MS (8000)
#define PREVIEW_GRAB_TIMEOUT_MS (15000)

// @class Preview
// Function | Purpose
// --- | ---
// Preview__binPath(dst, dst_sz) | Locate the camera_preview executable
// Preview__spawn(cam) | Start a detached preview daemon for one camera
// Preview__ping(cam) | Is a preview daemon already serving this camera?
// Preview__ensure(cam) | Start the daemon if it is not already running
// Preview__grab(cam, req, res) | Ask the daemon to freeze and write one frame
// Preview__die(cam) | Ask the daemon to shut down now

// Locate the camera_preview executable: next to this binary, or $CAMERA_PREVIEW_BIN
bool Preview__binPath(char* dst, u32 dst_sz) {
  const char* override = getenv("CAMERA_PREVIEW_BIN");
  if (NULL != override && 0 != override[0]) {
    return cstr__copy(dst, dst_sz, override);
  }

  char self[MAX_PATH];
  ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0) {
    LOG_ERRORF("cannot read /proc/self/exe to locate camera_preview");
    return false;
  }
  self[n] = 0;

  char dir[MAX_PATH];
  if (!File__dirnameOf(dir, (u32)sizeof(dir), self)) {
    return false;
  }
  int k = snprintf(dst, (size_t)dst_sz, "%s/camera_preview", dir);
  return k > 0 && (u32)k < dst_sz;
}

// Ping a preview daemon. Distinguishes three states, because "listening but
// still warming up" must NOT look like "not running" -- spawning a second
// daemon for a camera one already owns would just fail on the busy device.
//   out_ready = the daemon will hand over a frame worth analyzing
//   return    = a daemon is listening at all
static bool _Preview__ping(const Camera* cam, bool* out_ready) {
  if (NULL != out_ready) {
    *out_ready = false;
  }
  char sock[MAX_PATH];
  if (!Ipc__sockPath(sock, (u32)sizeof(sock), cam)) {
    return false;
  }
  int fd = -1;
  if (!Ipc__connect(sock, &fd)) {
    return false;
  }

  IpcReq req = {.kind = IPC_REQ_PING, .quality = 90};
  char line[1024];
  u32 len = 0;
  bool answered = Ipc__reqEncode(&req, line, (u32)sizeof(line)) && Ipc__writeLine(fd, line) &&
                  Ipc__readLine(fd, line, (u32)sizeof(line), 3000, &len);
  (void)close(fd);
  if (!answered) {
    return false;
  }

  // the response is small, so a stack arena is enough for its strings
  u8 scratch_buf[2048];
  Arena scratch = {
      .buf = scratch_buf,
      .pos = scratch_buf,
      .end = scratch_buf + sizeof(scratch_buf)};
  IpcRes res = {0};
  if (!Ipc__resDecode(line, len, &scratch, &res)) {
    return true;  // listening, but speaking something we do not understand
  }
  if (NULL != out_ready) {
    *out_ready = res.ok;
  }
  return true;
}

// Is a preview daemon listening for this camera?
bool Preview__alive(const Camera* cam) {
  return _Preview__ping(cam, NULL);
}

// Is a preview daemon ready to hand over a frame?
bool Preview__ready(const Camera* cam) {
  bool ready = false;
  (void)_Preview__ping(cam, &ready);
  return ready;
}

// Start a detached preview daemon for one camera
bool Preview__spawn(const Camera* cam) {
  char bin[MAX_PATH];
  if (!Preview__binPath(bin, (u32)sizeof(bin))) {
    return false;
  }
  if (!File__exists(bin)) {
    LOG_ERRORF("camera_preview not found at %s (set CAMERA_PREVIEW_BIN)", bin);
    return false;
  }

  char idx[16];
  (void)snprintf(idx, sizeof(idx), "%lu", (unsigned long)cam->index);

  pid_t pid = fork();
  if (pid < 0) {
    LOG_ERRORF("fork() failed while starting camera_preview");
    return false;
  }
  char log[MAX_PATH];
  bool have_log = Ipc__logPath(log, (u32)sizeof(log), cam);

  if (0 == pid) {
    // child: detach from this session so the daemon survives the MCP client
    (void)setsid();
    // stdin/stdout are the MCP transport; the daemon must not inherit them.
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      (void)dup2(devnull, STDOUT_FILENO);
      if (devnull > STDERR_FILENO) {
        (void)close(devnull);
      }
    }
    // stderr must be redirected too, and not merely for tidiness: a detached
    // daemon holding the parent's stderr PIPE open keeps that pipe unclosed
    // for its whole lifetime, so an MCP host that reads our stderr would block
    // long after camera_mcp exited. Give the daemon its own log file instead.
    int fd = have_log ? open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
    if (fd < 0) {
      fd = open("/dev/null", O_WRONLY);
    }
    if (fd >= 0) {
      (void)dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) {
        (void)close(fd);
      }
    }
    char* argv[] = {bin, (char*)"--camera", idx, NULL};
    (void)execv(bin, argv);
    // only reached when exec fails
    Console__error("[camera] ERROR execv(%s) failed\n", bin);
    _exit(127);
  }

  // parent: wait for the daemon to start serving, rather than guessing
  u64 deadline = Time__now() + PREVIEW_START_TIMEOUT_MS;
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    if (Preview__ready(cam)) {
      LOG_INFOF("camera_preview ready for %s (log: %s)", cam->name, have_log ? log : "none");
      return true;
    }
    // if the child already died, stop waiting on it
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      LOG_ERRORF(
          "camera_preview exited during startup (status %d); see %s",
          status,
          have_log ? log : "its output");
      return false;
    }
    if (Time__now() > deadline) {
      LOG_ERRORF(
          "camera_preview did not become ready within %d ms; see %s",
          PREVIEW_START_TIMEOUT_MS,
          have_log ? log : "its output");
      return false;
    }
    Time__sleep_ms(50);
  }
  return false;
}

// Start the daemon if it is not already running.
// A daemon that is listening but not yet warmed up is waited on, never
// duplicated: only one process can stream from a V4L2 device.
bool Preview__ensure(const Camera* cam) {
  if (!Preview__alive(cam)) {
    return Preview__spawn(cam);
  }
  u64 deadline = Time__now() + PREVIEW_START_TIMEOUT_MS;
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    if (Preview__ready(cam)) {
      return true;
    }
    if (!Preview__alive(cam)) {
      return Preview__spawn(cam);  // it went away while we waited
    }
    if (Time__now() > deadline) {
      LOG_ERRORF("the running camera_preview never became ready");
      return false;
    }
    Time__sleep_ms(50);
  }
  return false;
}

// send one request and read one response over a fresh connection
static bool _Preview__rpc(
    const Camera* cam, const IpcReq* req, u32 timeout_ms, Arena* scratch, IpcRes* res) {
  char sock[MAX_PATH];
  if (!Ipc__sockPath(sock, (u32)sizeof(sock), cam)) {
    return false;
  }
  int fd = -1;
  if (!Ipc__connect(sock, &fd)) {
    LOG_ERRORF("no preview listening on %s", sock);
    return false;
  }

  char* line = (char*)Arena__push(scratch, MAX_JSON_BYTES);
  if (!Ipc__reqEncode(req, line, MAX_JSON_BYTES) || !Ipc__writeLine(fd, line)) {
    (void)close(fd);
    return false;
  }
  u32 len = 0;
  bool got = Ipc__readLine(fd, line, MAX_JSON_BYTES, timeout_ms, &len);
  (void)close(fd);
  if (!got) {
    return false;
  }
  return Ipc__resDecode(line, len, scratch, res);
}

// Ask the daemon to freeze and write one frame
bool Preview__grab(const Camera* cam, const IpcReq* req, Arena* scratch, IpcRes* res) {
  return _Preview__rpc(cam, req, PREVIEW_GRAB_TIMEOUT_MS, scratch, res);
}

// Ask the daemon to shut down now
bool Preview__die(const Camera* cam, Arena* scratch) {
  IpcReq req = {.kind = IPC_REQ_DIE, .quality = 90};
  IpcRes res = {0};
  return _Preview__rpc(cam, &req, 3000, scratch, &res);
}
