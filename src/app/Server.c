#pragma once

#include <fcntl.h>  // IWYU pragma: keep // fcntl()
#include <sys/socket.h>  // IWYU pragma: keep // accept()
#include <unistd.h>  // IWYU pragma: keep // close(), unlink()

#include "../unity.h"  // IWYU pragma: keep

// The preview daemon's request handler, running on its own thread.
//
// It depends on the `_G` Preview__State declared by preview.c, which owns the
// buffers all three threads share. The contract with the render thread is
// narrow and one-way:
//   - this thread fills `frozen` and raises `showing_frozen`, `freeze_until_ms`
//     and `sfx_pending`
//   - the render thread consumes them and lowers `showing_frozen` when the
//     freeze expires
// Everything touching `live`/`frozen` is under `frame_mtx`.
//
// The frame handed back is copied out of the SAME locked snapshot that becomes
// the frozen on-screen image, so the JPEG the agent analyses and the frame the
// user watches freeze are byte-identical. That equality is the whole point of
// the window.

#define SERVER_FREEZE_MS (2000)  // how long the captured frame is held still
#define SERVER_TOSS_MS (650)  // how long it takes to be thrown away afterwards
#define SERVER_ACCEPT_POLL_MS (20)
// How long a grab waits for the camera to produce a usable frame. A cold UVC
// device needs a few hundred milliseconds to stream and settle its exposure,
// and a grab that arrives in that window should wait rather than fail.
#define SERVER_FRAME_WAIT_MS (5000)
#define SERVER_FRAME_POLL_MS (10)

// @class Server
// Function | Purpose
// --- | ---
// Server__start() | Bind the socket and start the accept thread
// Server__stop() | Stop accepting and remove the socket
// Server__defaultPath(dst, dst_sz, seq) | Build a transient frame path

// Build a transient frame path: $CAMERA_DIR/YYYYMMDD/<unix_ms>-<seq>.jpg
bool Server__defaultPath(char* dst, u32 dst_sz, u64 seq) {
  const char* base = getenv("CAMERA_DIR");
  if (NULL == base || 0 == base[0]) {
    base = "/tmp/camera";
  }
  s64 now = Time__unix_ms();
  char day[16];
  if (!Time__ymd(now, day, (u32)sizeof(day))) {
    (void)cstr__copy(day, (u32)sizeof(day), "00000000");
  }
  int n = snprintf(
      dst,
      (size_t)dst_sz,
      "%s/%s/%lld-%llu.jpg",
      base,
      day,
      (long long)now,
      (unsigned long long)seq);
  return n > 0 && (u32)n < dst_sz;
}

// Freeze the current frame, write it as a JPEG, and report where it went.
// Returns with `res` always populated, including on failure.
static void _Server__grab(const IpcReq* req, IpcRes* res) {
  memset(res, 0, sizeof(*res));
  res->captured_at = Time__unix_ms();
  res->persist = req->persist;

  u64 frame_bytes = V4l2__frameBytes(&_G->cap);

  // --- wait for a settled frame, rather than returning a black one ---
  u64 deadline = Time__now() + SERVER_FRAME_WAIT_MS;
  for (u32 i = 0; i < MAX_LOOP_ITER && !_G->ready && !_G->quitting; i++) {
    if (Time__now() > deadline) {
      break;
    }
    Time__sleep_ms(SERVER_FRAME_POLL_MS);
  }

  // --- take one atomic snapshot, and make it the frozen frame ---
  Mutex__lock(&_G->frame_mtx);
  if (!_G->live_valid) {
    Mutex__unlock(&_G->frame_mtx);
    (void)snprintf(
        res->error,
        sizeof(res->error),
        "camera produced no frame within %d ms",
        SERVER_FRAME_WAIT_MS);
    return;
  }
  if (!_G->ready) {
    // the exposure never settled, but a late frame still beats no frame
    LOG_ERRORF("grabbing before warmup finished; the frame may be underexposed");
  }
  memcpy(_G->work, _G->live, (size_t)frame_bytes);  // the copy we encode
  memcpy(_G->frozen, _G->live, (size_t)frame_bytes);  // the copy we display
  _G->frozen_valid = true;
  _G->frozen_dirty = true;  // tells the render thread to take this new snapshot
  _G->anim_active = true;
  _G->freeze_until_ms = Time__now() + SERVER_FREEZE_MS;
  _G->toss_until_ms = _G->freeze_until_ms + SERVER_TOSS_MS;
  _G->sfx_pending = true;
  _G->last_grab_ms = Time__now();
  u64 seq = ++_G->grab_seq;
  Mutex__unlock(&_G->frame_mtx);

  res->width = _G->cap.width;
  res->height = _G->cap.height;

  // --- encode outside the lock; the capture thread keeps running ---
  u64 cap_bytes = Jpeg__boundLen(_G->cap.width, _G->cap.height);
  if (cap_bytes > _G->jpeg_cap) {
    (void)cstr__copy(res->error, (u32)sizeof(res->error), "jpeg buffer is undersized");
    return;
  }
  u64 len = Jpeg__encodeYuyv(
      _G->jpeg,
      _G->jpeg_cap,
      _G->work,
      _G->cap.width,
      _G->cap.height,
      req->quality);
  if (0 == len) {
    (void)cstr__copy(res->error, (u32)sizeof(res->error), "jpeg encoding failed");
    return;
  }

  // --- decide where it lives ---
  if (0 != req->file[0]) {
    (void)cstr__copy(res->path, (u32)sizeof(res->path), req->file);
  } else if (!Server__defaultPath(res->path, (u32)sizeof(res->path), seq)) {
    (void)cstr__copy(res->error, (u32)sizeof(res->error), "could not build a frame path");
    return;
  }

  if (!File__mkdirpFor(res->path)) {
    (void)snprintf(res->error, sizeof(res->error), "cannot create the directory for %s", res->path);
    return;
  }
  if (!File__writeAll(res->path, _G->jpeg, len)) {
    (void)snprintf(res->error, sizeof(res->error), "cannot write %s", res->path);
    return;
  }

  // --- track it for the idle sweep ---
  Mutex__lock(&_G->reg_mtx);
  bool tracked = Registry__add(&_G->reg, res->path, req->persist);
  Mutex__unlock(&_G->reg_mtx);
  if (!tracked && !req->persist) {
    // we could not record it, so we cannot promise to clean it up later
    LOG_ERRORF("%s will not be swept; treat it as persisted", res->path);
    res->persist = true;
  }

  res->bytes = (u32)len;
  res->ok = true;
  LOG_INFOF(
      "grabbed %s (%lu bytes, quality %lu, persist=%s)",
      res->path,
      (unsigned long)len,
      (unsigned long)req->quality,
      res->persist ? "true" : "false");
}

// serve exactly one request on an accepted connection
static void _Server__serveOne(int fd) {
  Arena__reset(_G->ipc_arena);
  char* line = (char*)Arena__push(_G->ipc_arena, MAX_JSON_BYTES);

  u32 len = 0;
  if (!Ipc__readLine(fd, line, MAX_JSON_BYTES, 5000, &len)) {
    return;
  }

  IpcReq req;
  if (!Ipc__reqDecode(line, len, _G->ipc_arena, &req)) {
    IpcRes res = {0};
    (void)cstr__copy(res.error, (u32)sizeof(res.error), "malformed request");
    if (Ipc__resEncode(&res, line, MAX_JSON_BYTES)) {
      (void)Ipc__writeLine(fd, line);
    }
    return;
  }

  IpcRes res;
  memset(&res, 0, sizeof(res));
  switch (req.kind) {
    case IPC_REQ_PING:
      // ok means "ready to hand over a frame worth analyzing", so a client
      // waiting on startup waits through warmup too
      res.ok = _G->ready;
      res.width = _G->cap.width;
      res.height = _G->cap.height;
      res.captured_at = Time__unix_ms();
      if (!res.ok) {
        (void)cstr__copy(res.error, (u32)sizeof(res.error), "still warming up");
      }
      break;
    case IPC_REQ_GRAB:
      _Server__grab(&req, &res);
      break;
    case IPC_REQ_DIE:
      res.ok = true;
      LOG_INFOF("shutdown requested over ipc");
      // reply before quitting so the client is not left waiting
      if (Ipc__resEncode(&res, line, MAX_JSON_BYTES)) {
        (void)Ipc__writeLine(fd, line);
      }
      _G->quitting = true;
      return;
    default:
      (void)cstr__copy(res.error, (u32)sizeof(res.error), "unknown request kind");
      break;
  }

  if (Ipc__resEncode(&res, line, MAX_JSON_BYTES)) {
    (void)Ipc__writeLine(fd, line);
  } else {
    LOG_ERRORF("response did not fit; dropping it");
  }
}

// accept connections until the daemon is asked to quit
static void* _Server__acceptLoop(void* userdata) {
  (void)userdata;
  LOG_INFOF("ipc listening on %s", _G->sock_path);

  while (!_G->quitting) {
    int fd = accept(_G->listen_fd, NULL, NULL);
    if (fd < 0) {
      if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno) {
        // the listen socket is non-blocking so this loop can notice `quitting`
        Time__sleep_ms(SERVER_ACCEPT_POLL_MS);
        continue;
      }
      LOG_ERRORF("accept() failed (errno %d); stopping the ipc thread", errno);
      break;
    }
    _Server__serveOne(fd);
    (void)close(fd);
  }

  LOG_DEBUGF("ipc thread exiting");
  return NULL;
}

// Bind the socket and start the accept thread
bool Server__start(void) {
  if (!Ipc__sockPath(_G->sock_path, (u32)sizeof(_G->sock_path), &_G->cam)) {
    LOG_ERRORF("cannot determine the ipc socket path");
    return false;
  }
  if (!Ipc__listen(_G->sock_path, &_G->listen_fd)) {
    return false;
  }
  // non-blocking accept, so the loop can poll `quitting`
  int flags = fcntl(_G->listen_fd, F_GETFL, 0);
  if (flags < 0 || 0 != fcntl(_G->listen_fd, F_SETFL, flags | O_NONBLOCK)) {
    LOG_ERRORF("cannot make the listen socket non-blocking");
    (void)close(_G->listen_fd);
    _G->listen_fd = -1;
    return false;
  }
  if (!Thread__create(&_G->ipc_thread, _Server__acceptLoop, NULL)) {
    LOG_ERRORF("cannot start the ipc thread");
    (void)close(_G->listen_fd);
    _G->listen_fd = -1;
    return false;
  }
  return true;
}

// Stop accepting and remove the socket
void Server__stop(void) {
  _G->quitting = true;
  Thread__join(&_G->ipc_thread);
  if (_G->listen_fd >= 0) {
    (void)close(_G->listen_fd);
    _G->listen_fd = -1;
  }
  if (0 != _G->sock_path[0]) {
    (void)unlink(_G->sock_path);
  }
}
