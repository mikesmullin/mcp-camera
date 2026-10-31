#pragma once

#include <sys/socket.h>  // IWYU pragma: keep // socket(), bind()
#include <sys/un.h>  // IWYU pragma: keep // sockaddr_un
#include <unistd.h>  // IWYU pragma: keep // close(), read(), write()

#include "../unity.h"  // IWYU pragma: keep

// Transport between camera_mcp (client) and camera_preview (server).
//
// A Unix domain stream socket carrying newline-delimited JSON, one request and
// one response per connection. There is one socket per camera, so each camera
// gets its own preview window and its own lifetime.
//
// Frames themselves never cross the socket: the preview writes the JPEG to disk
// and replies with the path, which the client then reads. That keeps the wire
// format pure text and makes the file the single source of truth for both
// processes.

// @class Ipc
// Function | Purpose
// --- | ---
// Ipc__sockPath(dst, dst_sz, cam) | Per-camera socket path for this user
// Ipc__listen(path, out_fd) | Bind and listen, clearing a stale socket first
// Ipc__connect(path, out_fd) | Connect to a listening preview, or fail fast
// Ipc__readLine(fd, buf, cap, timeout_ms, out_len) | Read one newline-terminated line
// Ipc__writeLine(fd, line) | Write one line, appending the newline
// Ipc__reqEncode(req, buf, cap) | Serialize a request
// Ipc__reqDecode(line, len, scratch, out) | Parse a request
// Ipc__resEncode(res, buf, cap) | Serialize a response
// Ipc__resDecode(line, len, scratch, out) | Parse a response

// forward declaration: Ipc__listen() probes for a live owner before unlinking
bool Ipc__connect(const char* path, int* out_fd);

// Per-camera socket path for this user. Prefers $XDG_RUNTIME_DIR (tmpfs, 0700,
// cleaned on logout) and falls back to a uid-scoped directory under /tmp.
bool Ipc__sockPath(char* dst, u32 dst_sz, const Camera* cam) {
  const char* runtime = getenv("XDG_RUNTIME_DIR");
  char dir[MAX_PATH];
  if (NULL != runtime && 0 != runtime[0]) {
    (void)snprintf(dir, sizeof(dir), "%s/mcp-camera", runtime);
  } else {
    (void)snprintf(dir, sizeof(dir), "/tmp/mcp-camera-%lu", (unsigned long)getuid());
  }
  if (!File__mkdirp(dir)) {
    return false;
  }
  // sun_path is short (~108 bytes), so key on the node index only
  int n = snprintf(dst, (size_t)dst_sz, "%s/video%lu.sock", dir, (unsigned long)cam->index);
  return n > 0 && (u32)n < dst_sz;
}

// Bind and listen, clearing a stale socket first
bool Ipc__listen(const char* path, int* out_fd) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (!cstr__copy(addr.sun_path, (u32)sizeof(addr.sun_path), path)) {
    LOG_ERRORF("socket path too long: %s", path);
    return false;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERRORF("socket() failed");
    return false;
  }

  // A leftover socket file from a killed preview would block bind(); it is safe
  // to remove because a live preview holds an exclusive listen on it, which we
  // test for by connecting first.
  int probe = -1;
  if (Ipc__connect(path, &probe)) {
    (void)close(probe);
    (void)close(fd);
    LOG_ERRORF("another preview already owns %s", path);
    return false;
  }
  (void)unlink(path);

  if (0 != bind(fd, (struct sockaddr*)&addr, sizeof(addr))) {
    LOG_ERRORF("bind() failed on %s", path);
    (void)close(fd);
    return false;
  }
  if (0 != listen(fd, 8)) {
    LOG_ERRORF("listen() failed on %s", path);
    (void)close(fd);
    return false;
  }
  *out_fd = fd;
  return true;
}

// Connect to a listening preview, or fail fast
bool Ipc__connect(const char* path, int* out_fd) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (!cstr__copy(addr.sun_path, (u32)sizeof(addr.sun_path), path)) {
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  if (0 != connect(fd, (struct sockaddr*)&addr, sizeof(addr))) {
    (void)close(fd);
    return false;
  }
  *out_fd = fd;
  return true;
}

// Read one newline-terminated line, waiting at most timeout_ms in total
bool Ipc__readLine(int fd, char* buf, u32 cap, u32 timeout_ms, u32* out_len) {
  u32 len = 0;
  u64 deadline = Time__now() + (u64)timeout_ms;
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    if (len + 1 >= cap) {
      LOG_ERRORF("ipc: line exceeded %lu bytes", (unsigned long)cap);
      return false;
    }
    ssize_t n = read(fd, buf + len, 1);
    if (1 == n) {
      if ('\n' == buf[len]) {
        buf[len] = 0;
        if (NULL != out_len) {
          *out_len = len;
        }
        return true;
      }
      len++;
      continue;
    }
    if (0 == n) {
      LOG_DEBUGF("ipc: peer closed after %lu bytes", (unsigned long)len);
      return false;  // EOF before a newline
    }
    if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno) {
      if (Time__now() > deadline) {
        LOG_ERRORF("ipc: read timed out after %lu ms", (unsigned long)timeout_ms);
        return false;
      }
      Time__sleep_ms(2);
      continue;
    }
    LOG_ERRORF("ipc: read failed (errno %d)", errno);
    return false;
  }
  return false;
}

// Write one line, appending the newline
bool Ipc__writeLine(int fd, const char* line) {
  u32 len = cstr__len(line);
  u32 sent = 0;
  for (u32 i = 0; i < MAX_LOOP_ITER && sent < len; i++) {
    ssize_t n = write(fd, line + sent, (size_t)(len - sent));
    if (n > 0) {
      sent += (u32)n;
      continue;
    }
    if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)) {
      Time__sleep_ms(2);
      continue;
    }
    LOG_ERRORF("ipc: write failed (errno %d)", errno);
    return false;
  }
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    ssize_t n = write(fd, "\n", 1);
    if (1 == n) {
      return true;
    }
    if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)) {
      Time__sleep_ms(2);
      continue;
    }
    return false;
  }
  return false;
}

// map a request kind to its wire token
static const char* _Ipc__kindName(IpcReqKind kind) {
  switch (kind) {
    case IPC_REQ_PING:
      return "ping";
    case IPC_REQ_GRAB:
      return "grab";
    case IPC_REQ_DIE:
      return "die";
    default:
      return "none";
  }
}

// Serialize a request
bool Ipc__reqEncode(const IpcReq* req, char* buf, u32 cap) {
  JsonW w;
  JsonW__init(&w, buf, cap);
  JsonW__objBegin(&w);
  JsonW__kstr(&w, "t", _Ipc__kindName(req->kind));
  JsonW__kstr(&w, "file", req->file);
  JsonW__kbool(&w, "persist", req->persist);
  JsonW__ku32(&w, "quality", req->quality);
  JsonW__objEnd(&w);
  return JsonW__ok(&w);
}

// Parse a request
bool Ipc__reqDecode(const char* line, u32 len, Arena* scratch, IpcReq* out) {
  memset(out, 0, sizeof(*out));
  out->quality = 90;

  Json j;
  Json__init(&j, line, len, scratch);
  if (!Json__object_begin(&j)) {
    return false;
  }
  JsonStr key = {0};
  for (u32 i = 0; i < MAX_LOOP_ITER && Json__object_key(&j, &key); i++) {
    if (JsonStr__eq(key, "t")) {
      char kind[16] = {0};
      if (!Json__stringTo(&j, kind, (u32)sizeof(kind))) {
        return false;
      }
      if (cstr__eq(kind, "ping")) {
        out->kind = IPC_REQ_PING;
      } else if (cstr__eq(kind, "grab")) {
        out->kind = IPC_REQ_GRAB;
      } else if (cstr__eq(kind, "die")) {
        out->kind = IPC_REQ_DIE;
      } else {
        out->kind = IPC_REQ_NONE;
      }
    } else if (JsonStr__eq(key, "file")) {
      if (!Json__stringTo(&j, out->file, (u32)sizeof(out->file))) {
        return false;
      }
    } else if (JsonStr__eq(key, "persist")) {
      if (!Json__bool(&j, &out->persist)) {
        return false;
      }
    } else if (JsonStr__eq(key, "quality")) {
      if (!Json__u32(&j, &out->quality)) {
        return false;
      }
    } else if (!Json__skipValue(&j)) {
      return false;
    }
  }
  if (!Json__object_end(&j) || j.failed) {
    return false;
  }
  if (out->quality < 1 || out->quality > 100) {
    out->quality = 90;
  }
  return IPC_REQ_NONE != out->kind;
}

// Serialize a response
bool Ipc__resEncode(const IpcRes* res, char* buf, u32 cap) {
  JsonW w;
  JsonW__init(&w, buf, cap);
  JsonW__objBegin(&w);
  JsonW__kbool(&w, "ok", res->ok);
  JsonW__kstr(&w, "path", res->path);
  JsonW__ku32(&w, "width", res->width);
  JsonW__ku32(&w, "height", res->height);
  JsonW__ku32(&w, "bytes", res->bytes);
  JsonW__ks64(&w, "capturedAt", res->captured_at);
  JsonW__kbool(&w, "persist", res->persist);
  JsonW__kstr(&w, "error", res->error);
  JsonW__objEnd(&w);
  return JsonW__ok(&w);
}

// Parse a response
bool Ipc__resDecode(const char* line, u32 len, Arena* scratch, IpcRes* out) {
  memset(out, 0, sizeof(*out));

  Json j;
  Json__init(&j, line, len, scratch);
  if (!Json__object_begin(&j)) {
    return false;
  }
  JsonStr key = {0};
  for (u32 i = 0; i < MAX_LOOP_ITER && Json__object_key(&j, &key); i++) {
    if (JsonStr__eq(key, "ok")) {
      if (!Json__bool(&j, &out->ok)) {
        return false;
      }
    } else if (JsonStr__eq(key, "path")) {
      if (!Json__stringTo(&j, out->path, (u32)sizeof(out->path))) {
        return false;
      }
    } else if (JsonStr__eq(key, "width")) {
      if (!Json__u32(&j, &out->width)) {
        return false;
      }
    } else if (JsonStr__eq(key, "height")) {
      if (!Json__u32(&j, &out->height)) {
        return false;
      }
    } else if (JsonStr__eq(key, "bytes")) {
      if (!Json__u32(&j, &out->bytes)) {
        return false;
      }
    } else if (JsonStr__eq(key, "capturedAt")) {
      f64 v = 0.0;
      if (!Json__number(&j, &v)) {
        return false;
      }
      out->captured_at = (s64)v;
    } else if (JsonStr__eq(key, "persist")) {
      if (!Json__bool(&j, &out->persist)) {
        return false;
      }
    } else if (JsonStr__eq(key, "error")) {
      if (!Json__stringTo(&j, out->error, (u32)sizeof(out->error))) {
        return false;
      }
    } else if (!Json__skipValue(&j)) {
      return false;
    }
  }
  return Json__object_end(&j) && !j.failed;
}
