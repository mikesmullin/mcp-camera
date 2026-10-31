#pragma once

#include <arpa/inet.h>  // IWYU pragma: keep // inet_pton()
#include <netinet/in.h>  // IWYU pragma: keep // sockaddr_in
#include <netinet/tcp.h>  // IWYU pragma: keep // TCP_NODELAY
#include <sys/select.h>  // IWYU pragma: keep // select()
#include <sys/socket.h>  // IWYU pragma: keep // socket()
#include <unistd.h>  // IWYU pragma: keep // close()

#include "../unity.h"  // IWYU pragma: keep

// A single-shot HTTP/1.1 POST client, just enough for the two local inference
// endpoints. No libcurl, no keep-alive, no TLS: both backends listen on
// loopback.
//
// Response bodies arrive either with a Content-Length (what both backends send
// today), chunked, or delimited by the close -- all three are handled so a
// backend upgrade cannot silently break capture.

// @class Http
// Function | Purpose
// --- | ---
// Http__parseUrl(url, host, host_sz, port, path, path_sz) | Split "http://host:port/path"
// Http__post(host, port, path, body, body_len, arena, out) | POST a body, return the response

typedef struct {
  u32 status;  // HTTP status code
  const char* body;  // NUL-terminated, arena-owned
  u32 body_len;
} HttpRes;

// Split "http://host:port/path" into its parts; defaults port 80 and path "/"
bool Http__parseUrl(const char* url, char* host, u32 host_sz, u16* port, char* path, u32 path_sz) {
  const char* p = url;
  if (cstr__eqn(7, p, "http://")) {
    p += 7;
  } else if (cstr__eqn(8, p, "https://")) {
    LOG_ERRORF("https is not supported: %s", url);
    return false;
  }

  // authority runs to the first '/' (or the end)
  u32 auth_len = 0;
  while (0 != p[auth_len] && '/' != p[auth_len] && auth_len < MAX_LOOP_ITER) {
    auth_len++;
  }

  // split host:port
  u32 colon = auth_len;
  for (u32 i = 0; i < auth_len; i++) {
    if (':' == p[i]) {
      colon = i;
    }
  }
  if (!cstr__copyn(host, host_sz, p, colon)) {
    return false;
  }
  *port = 80;
  if (colon < auth_len) {
    char tmp[8];
    if (!cstr__copyn(tmp, (u32)sizeof(tmp), p + colon + 1, auth_len - colon - 1)) {
      return false;
    }
    u32 v = 0;
    if (!cstr__toU32(tmp, &v) || 0 == v || v > 65535) {
      LOG_ERRORF("bad port in url: %s", url);
      return false;
    }
    *port = (u16)v;
  }

  const char* rest = p + auth_len;
  return cstr__copy(path, path_sz, 0 == rest[0] ? "/" : rest);
}

// connect a TCP socket to host:port, with a bounded wait
static bool _Http__connect(const char* host, u16 port, u32 timeout_ms, int* out_fd) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (1 != inet_pton(AF_INET, host, &addr.sin_addr)) {
    // only numeric loopback/LAN addresses are supported; no resolver here
    LOG_ERRORF("host must be a numeric IPv4 address, got '%s'", host);
    return false;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERRORF("socket() failed");
    return false;
  }
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  struct timeval tv;
  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (0 != connect(fd, (struct sockaddr*)&addr, sizeof(addr))) {
    LOG_ERRORF("cannot reach %s:%lu (errno %d)", host, (unsigned long)port, errno);
    (void)close(fd);
    return false;
  }
  *out_fd = fd;
  return true;
}

// write a whole buffer, tolerating partial writes
static bool _Http__sendAll(int fd, const u8* buf, u64 len) {
  u64 sent = 0;
  for (u32 i = 0; i < MAX_LOOP_ITER && sent < len; i++) {
    ssize_t n = write(fd, buf + sent, (size_t)(len - sent));
    if (n > 0) {
      sent += (u64)n;
      continue;
    }
    if (n < 0 && (EINTR == errno || EAGAIN == errno)) {
      continue;
    }
    LOG_ERRORF("http: send failed after %llu bytes (errno %d)", (unsigned long long)sent, errno);
    return false;
  }
  return sent == len;
}

// read until EOF or the cap, into an arena-backed buffer
static bool _Http__recvAll(int fd, Arena* arena, u8** out, u64* out_len) {
  u8* buf = (u8*)Arena__push(arena, (u64)MAX_HTTP_BODY + 1ULL);
  u64 len = 0;
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    if (len >= MAX_HTTP_BODY) {
      LOG_ERRORF("http: response exceeded %lu bytes", (unsigned long)MAX_HTTP_BODY);
      return false;
    }
    ssize_t n = read(fd, buf + len, (size_t)(MAX_HTTP_BODY - len));
    if (n > 0) {
      len += (u64)n;
      continue;
    }
    if (0 == n) {
      break;  // peer closed
    }
    if (EINTR == errno) {
      continue;
    }
    if (EAGAIN == errno || EWOULDBLOCK == errno) {
      LOG_ERRORF("http: read timed out after %llu bytes", (unsigned long long)len);
      return false;
    }
    LOG_ERRORF("http: read failed (errno %d)", errno);
    return false;
  }
  buf[len] = 0;
  *out = buf;
  *out_len = len;
  return true;
}

// find the end of the header block; returns the body offset (0 when absent)
static u64 _Http__headerEnd(const u8* buf, u64 len) {
  for (u64 i = 0; i + 3 < len && i < MAX_LOOP_ITER; i++) {
    if ('\r' == buf[i] && '\n' == buf[i + 1] && '\r' == buf[i + 2] && '\n' == buf[i + 3]) {
      return i + 4;
    }
  }
  // tolerate bare-LF header terminators
  for (u64 i = 0; i + 1 < len && i < MAX_LOOP_ITER; i++) {
    if ('\n' == buf[i] && '\n' == buf[i + 1]) {
      return i + 2;
    }
  }
  return 0;
}

// is `name:` present in the (lowercased comparison) header block?
static bool _Http__headerIs(const char* headers, const char* name, const char* value) {
  u32 nlen = cstr__len(name);
  for (u32 i = 0; 0 != headers[i] && i < MAX_LOOP_ITER; i++) {
    if ((0 == i || '\n' == headers[i - 1]) && cstr__ieqn(nlen, headers + i, name)) {
      const char* v = headers + i + nlen;
      while (' ' == *v || ':' == *v) {
        v++;
      }
      return cstr__ieqn(cstr__len(value), v, value);
    }
  }
  return false;
}

// read a header's value as a u32 (0 when missing or malformed)
static u32 _Http__headerU32(const char* headers, const char* name) {
  u32 nlen = cstr__len(name);
  for (u32 i = 0; 0 != headers[i] && i < MAX_LOOP_ITER; i++) {
    if ((0 == i || '\n' == headers[i - 1]) && cstr__ieqn(nlen, headers + i, name)) {
      const char* v = headers + i + nlen;
      while (' ' == *v || ':' == *v) {
        v++;
      }
      u32 out = 0;
      bool any = false;
      while (*v >= '0' && *v <= '9') {
        out = out * 10u + (u32)(*v - '0');
        v++;
        any = true;
      }
      return any ? out : 0;
    }
  }
  return 0;
}

// decode a chunked body in place; returns the decoded length
static bool _Http__dechunk(u8* body, u64 len, u64* out_len) {
  u64 r = 0, w = 0;
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    // parse the hex chunk size
    u64 sz = 0;
    bool any = false;
    while (r < len) {
      u8 ch = body[r];
      u64 d;
      if (ch >= '0' && ch <= '9') {
        d = (u64)(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        d = (u64)(ch - 'a') + 10ULL;
      } else if (ch >= 'A' && ch <= 'F') {
        d = (u64)(ch - 'A') + 10ULL;
      } else {
        break;
      }
      sz = sz * 16ULL + d;
      r++;
      any = true;
    }
    if (!any) {
      return false;
    }
    // skip any chunk extension, then the CRLF
    while (r < len && '\n' != body[r]) {
      r++;
    }
    if (r < len) {
      r++;
    }
    if (0 == sz) {
      *out_len = w;
      body[w] = 0;
      return true;
    }
    if (r + sz > len) {
      return false;  // truncated
    }
    memmove(body + w, body + r, (size_t)sz);
    w += sz;
    r += sz;
    // skip the CRLF that terminates the chunk data
    while (r < len && '\n' != body[r]) {
      r++;
    }
    if (r < len) {
      r++;
    }
  }
  return false;
}

// POST a body and return the response; status is set even for 4xx/5xx
bool Http__post(
    const char* host,
    u16 port,
    const char* path,
    const u8* body,
    u64 body_len,
    u32 timeout_ms,
    Arena* arena,
    HttpRes* out) {
  memset(out, 0, sizeof(*out));

  int fd = -1;
  if (!_Http__connect(host, port, timeout_ms, &fd)) {
    return false;
  }

  // request line + headers
  char head[512];
  int hn = snprintf(
      head,
      sizeof(head),
      "POST %s HTTP/1.1\r\n"
      "Host: %s:%lu\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %llu\r\n"
      "Accept: application/json\r\n"
      "Connection: close\r\n"
      "\r\n",
      path,
      host,
      (unsigned long)port,
      (unsigned long long)body_len);
  if (hn <= 0 || (size_t)hn >= sizeof(head)) {
    LOG_ERRORF("http: request headers too long for %s", path);
    (void)close(fd);
    return false;
  }

  bool sent = _Http__sendAll(fd, (const u8*)head, (u64)hn) && _Http__sendAll(fd, body, body_len);
  if (!sent) {
    (void)close(fd);
    return false;
  }

  u8* raw = NULL;
  u64 raw_len = 0;
  bool got = _Http__recvAll(fd, arena, &raw, &raw_len);
  (void)close(fd);
  if (!got) {
    return false;
  }

  // "HTTP/1.x NNN ..."
  if (raw_len < 12 || !cstr__eqn(5, (const char*)raw, "HTTP/")) {
    LOG_ERRORF("http: malformed response from %s:%lu", host, (unsigned long)port);
    return false;
  }
  out->status = (u32)((raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0'));

  u64 at = _Http__headerEnd(raw, raw_len);
  if (0 == at) {
    LOG_ERRORF("http: response had no header terminator");
    return false;
  }
  // NUL-terminate the header block so it can be scanned as a cstr
  char saved = (char)raw[at - 1];
  raw[at - 1] = 0;
  const char* headers = (const char*)raw;
  bool chunked = _Http__headerIs(headers, "transfer-encoding", "chunked");
  u32 declared = _Http__headerU32(headers, "content-length");
  raw[at - 1] = (u8)saved;

  u8* bodyp = raw + at;
  u64 blen = raw_len - at;
  if (chunked) {
    if (!_Http__dechunk(bodyp, blen, &blen)) {
      LOG_ERRORF("http: malformed chunked body");
      return false;
    }
  } else if (declared > 0) {
    if ((u64)declared > blen) {
      LOG_ERRORF(
          "http: truncated body (%llu of %lu bytes)",
          (unsigned long long)blen,
          (unsigned long)declared);
      return false;
    }
    blen = declared;
  }
  bodyp[blen] = 0;

  out->body = (const char*)bodyp;
  out->body_len = (u32)blen;
  return true;
}
