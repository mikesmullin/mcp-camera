#pragma once

#include <fcntl.h>  // IWYU pragma: keep // open()
#include <linux/videodev2.h>  // IWYU pragma: keep // VIDIOC_*
#include <sys/ioctl.h>  // IWYU pragma: keep // ioctl()
#include <sys/mman.h>  // IWYU pragma: keep // mmap()
#include <sys/select.h>  // IWYU pragma: keep // select()
#include <unistd.h>  // IWYU pragma: keep // close()

#include "../unity.h"  // IWYU pragma: keep

// Native V4L2 memory-mapped capture. No ffmpeg, no libv4l.
//
// The sequence the uvcvideo driver requires, in order:
//   S_FMT -> REQBUFS -> QUERYBUF + mmap + QBUF (each buffer) -> STREAMON
//   -> select() -> DQBUF -> (use frame) -> QBUF -> ... -> STREAMOFF
// Skipping the per-buffer QBUF before STREAMON makes STREAMON fail with
// EINVAL, which is the trap worth remembering here.
//
// Only YUYV (YUV 4:2:2 packed) is accepted: it is what this class of UVC
// webcam offers uncompressed, it converts to RGBA for display in a few
// instructions, and it feeds the JPEG encoder without a colour-space round
// trip.

#define V4L2_BUFFERS (4)  // enough to absorb a scheduling hiccup at 30fps

// @class V4l2
// Function | Purpose
// --- | ---
// V4l2__open(c, dev, want_w, want_h) | Open a device and negotiate a YUYV format
// V4l2__start(c) | Map buffers, queue them, and begin streaming
// V4l2__read(c, dst, dst_cap, timeout_ms) | Wait for and copy out the next frame
// V4l2__stop(c) | Stop streaming and unmap buffers
// V4l2__close(c) | Release the device
// V4l2__frameBytes(c) | Bytes one frame occupies

typedef struct {
  int fd;
  char path[MAX_PATH];
  u32 width, height;
  u32 n_buffers;
  u8* maps[V4L2_BUFFERS];
  u32 lens[V4L2_BUFFERS];
  bool streaming;
} V4l2Cap;

// Bytes one frame occupies (YUYV is exactly 2 bytes per pixel)
static inline u64 V4l2__frameBytes(const V4l2Cap* c) {
  return (u64)c->width * (u64)c->height * 2ULL;
}

// retry an ioctl across EINTR, which select()-driven code hits routinely
static int _V4l2__ioctl(int fd, unsigned long req, void* arg) {
  int r;
  for (u32 i = 0; i < MAX_LOOP_ITER; i++) {
    r = ioctl(fd, req, arg);
    if (!(r < 0 && EINTR == errno)) {
      return r;
    }
  }
  return r;
}

// Open a device and negotiate a YUYV format as close as possible to the request
bool V4l2__open(V4l2Cap* c, const char* dev, u32 want_w, u32 want_h) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  if (!cstr__copy(c->path, (u32)sizeof(c->path), dev)) {
    return false;
  }

  c->fd = open(dev, O_RDWR);  // blocking; select() provides the timeout
  if (c->fd < 0) {
    LOG_ERRORF("cannot open %s (errno %d); another process may hold the camera", dev, errno);
    return false;
  }

  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = want_w;
  fmt.fmt.pix.height = want_h;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (0 != _V4l2__ioctl(c->fd, VIDIOC_S_FMT, &fmt)) {
    LOG_ERRORF("S_FMT failed on %s (errno %d)", dev, errno);
    (void)close(c->fd);
    c->fd = -1;
    return false;
  }

  // the driver may hand back a nearby size; honour whatever it chose
  if (V4L2_PIX_FMT_YUYV != fmt.fmt.pix.pixelformat) {
    LOG_ERRORF("%s does not offer YUYV capture", dev);
    (void)close(c->fd);
    c->fd = -1;
    return false;
  }
  c->width = fmt.fmt.pix.width;
  c->height = fmt.fmt.pix.height;
  if (0 == c->width || 0 == c->height || c->width > MAX_FRAME_W || c->height > MAX_FRAME_H) {
    LOG_ERRORF("%s negotiated an unusable size %ux%u", dev, c->width, c->height);
    (void)close(c->fd);
    c->fd = -1;
    return false;
  }
  LOG_INFOF("%s opened at %ux%u YUYV", dev, c->width, c->height);
  return true;
}

// unmap whatever has been mapped so far (used on the error path too)
static void _V4l2__unmap(V4l2Cap* c) {
  for (u32 i = 0; i < c->n_buffers && i < V4L2_BUFFERS; i++) {
    if (NULL != c->maps[i]) {
      (void)munmap(c->maps[i], c->lens[i]);
      c->maps[i] = NULL;
      c->lens[i] = 0;
    }
  }
  c->n_buffers = 0;
}

// Map buffers, queue them, and begin streaming
bool V4l2__start(V4l2Cap* c) {
  if (c->fd < 0) {
    return false;
  }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = V4L2_BUFFERS;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (0 != _V4l2__ioctl(c->fd, VIDIOC_REQBUFS, &req)) {
    LOG_ERRORF("REQBUFS failed on %s (errno %d)", c->path, errno);
    return false;
  }
  if (req.count < 2) {
    LOG_ERRORF("%s granted only %lu buffers", c->path, (unsigned long)req.count);
    return false;
  }
  c->n_buffers = req.count > V4L2_BUFFERS ? V4L2_BUFFERS : req.count;

  for (u32 i = 0; i < c->n_buffers; i++) {
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (0 != _V4l2__ioctl(c->fd, VIDIOC_QUERYBUF, &b)) {
      LOG_ERRORF("QUERYBUF %lu failed (errno %d)", (unsigned long)i, errno);
      _V4l2__unmap(c);
      return false;
    }
    void* p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, (off_t)b.m.offset);
    if (MAP_FAILED == p) {
      LOG_ERRORF("mmap buffer %lu failed (errno %d)", (unsigned long)i, errno);
      _V4l2__unmap(c);
      return false;
    }
    c->maps[i] = (u8*)p;
    c->lens[i] = b.length;
    // IMPORTANT: every buffer must be queued BEFORE STREAMON, or STREAMON
    // fails with EINVAL on uvcvideo.
    if (0 != _V4l2__ioctl(c->fd, VIDIOC_QBUF, &b)) {
      LOG_ERRORF("QBUF %lu failed (errno %d)", (unsigned long)i, errno);
      _V4l2__unmap(c);
      return false;
    }
  }

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (0 != _V4l2__ioctl(c->fd, VIDIOC_STREAMON, &type)) {
    LOG_ERRORF("STREAMON failed on %s (errno %d)", c->path, errno);
    _V4l2__unmap(c);
    return false;
  }
  c->streaming = true;
  LOG_INFOF("%s streaming with %lu buffers", c->path, (unsigned long)c->n_buffers);
  return true;
}

// Wait for and copy out the next frame (YUYV, V4l2__frameBytes() long)
bool V4l2__read(V4l2Cap* c, u8* dst, u64 dst_cap, u32 timeout_ms) {
  if (!c->streaming || NULL == dst) {
    return false;
  }
  u64 need = V4l2__frameBytes(c);
  if (dst_cap < need) {
    LOG_ERRORF(
        "frame buffer too small: %llu < %llu",
        (unsigned long long)dst_cap,
        (unsigned long long)need);
    return false;
  }

  fd_set fds;
  struct timeval tv;
  FD_ZERO(&fds);
  FD_SET(c->fd, &fds);
  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
  int r = select(c->fd + 1, &fds, NULL, NULL, &tv);
  if (0 == r) {
    LOG_DEBUGF("frame wait timed out after %lu ms", (unsigned long)timeout_ms);
    return false;
  }
  if (r < 0) {
    if (EINTR == errno) {
      return false;  // caller loops
    }
    LOG_ERRORF("select() failed on %s (errno %d)", c->path, errno);
    return false;
  }

  struct v4l2_buffer b;
  memset(&b, 0, sizeof(b));
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  if (0 != _V4l2__ioctl(c->fd, VIDIOC_DQBUF, &b)) {
    if (EAGAIN == errno) {
      return false;  // no frame ready after all
    }
    LOG_ERRORF("DQBUF failed on %s (errno %d)", c->path, errno);
    return false;
  }
  if (b.index >= c->n_buffers) {
    LOG_ERRORF("driver returned buffer index %lu out of range", (unsigned long)b.index);
    return false;
  }

  // a short frame means a USB glitch; requeue and let the caller retry
  bool good = b.bytesused >= need;
  if (good) {
    memcpy(dst, c->maps[b.index], (size_t)need);
  } else {
    LOG_DEBUGF(
        "discarding short frame (%lu of %llu bytes)",
        (unsigned long)b.bytesused,
        (unsigned long long)need);
  }

  if (0 != _V4l2__ioctl(c->fd, VIDIOC_QBUF, &b)) {
    LOG_ERRORF("re-QBUF failed on %s (errno %d)", c->path, errno);
    return false;
  }
  return good;
}

// Stop streaming and unmap buffers
void V4l2__stop(V4l2Cap* c) {
  if (c->fd >= 0 && c->streaming) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)_V4l2__ioctl(c->fd, VIDIOC_STREAMOFF, &type);
    c->streaming = false;
  }
  _V4l2__unmap(c);
}

// Release the device
void V4l2__close(V4l2Cap* c) {
  V4l2__stop(c);
  if (c->fd >= 0) {
    (void)close(c->fd);
    c->fd = -1;
  }
}
