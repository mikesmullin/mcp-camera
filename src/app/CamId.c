#pragma once

#include <dirent.h>  // IWYU pragma: keep // opendir()
#include <fcntl.h>  // IWYU pragma: keep // open()
#include <linux/videodev2.h>  // IWYU pragma: keep // VIDIOC_QUERYCAP
#include <sys/ioctl.h>  // IWYU pragma: keep // ioctl()
#include <unistd.h>  // IWYU pragma: keep // close()

#include "../unity.h"  // IWYU pragma: keep

// Camera discovery with no subprocess and no udev dependency:
//   - /sys/class/video4linux/video*       -> which nodes exist, and their index
//   - VIDIOC_QUERYCAP on each node        -> card name, and whether it can capture
//   - walk up the sysfs device chain      -> the USB device's serial
//
// A UVC webcam typically exposes two /dev/videoN nodes: one for video capture
// and one for metadata only. Only the capture-capable node is listed.

// @class CamId
// Function | Purpose
// --- | ---
// CamId__list(out) | Enumerate capture-capable cameras, sorted by index
// CamId__resolve(list, id, out) | Match an id (index, full name, or path) to one camera
// CamId__nameOf(cam) | Rebuild a camera's "{index}__{card}__{serial}" name

// read a whole small sysfs attribute into dst, trimmed; false when absent
static bool _CamId__readAttr(const char* path, char* dst, u32 dst_sz) {
  FILE* f = fopen(path, "rb");
  if (NULL == f) {
    return false;
  }
  u32 n = 0;
  int c;
  while (n + 1 < dst_sz && EOF != (c = fgetc(f))) {
    dst[n++] = (char)c;
  }
  dst[n] = 0;
  (void)fclose(f);
  cstr__trimRight(dst);
  return n > 0;
}

// walk up the sysfs device chain looking for a USB "serial" attribute
static bool _CamId__findSerial(u32 index, char* dst, u32 dst_sz) {
  char link[MAX_PATH];
  (void)
      snprintf(link, sizeof(link), "/sys/class/video4linux/video%lu/device", (unsigned long)index);

  char real[MAX_PATH];
  if (NULL == realpath(link, real)) {
    return false;
  }

  // the USB interface sits a few levels below the USB device that owns the serial
  for (u32 up = 0; up < 6; up++) {
    char attr[MAX_PATH];
    (void)snprintf(attr, sizeof(attr), "%s/serial", real);
    if (_CamId__readAttr(attr, dst, dst_sz)) {
      return true;
    }
    // strip one trailing path component
    u32 n = cstr__len(real);
    u32 cut = 0;
    for (u32 i = 1; i < n && i < MAX_LOOP_ITER; i++) {
      if ('/' == real[i]) {
        cut = i;
      }
    }
    if (0 == cut) {
      break;
    }
    real[cut] = 0;
  }
  dst[0] = 0;
  return false;
}

// Rebuild a camera's "{index}__{card}__{serial}" name.
// Each component is lowercased and reduced to [a-z0-9_] so the whole id is a
// safe, portable slug -- usable verbatim in a filename, a shell word or a URL.
// The double underscore stays as the component separator, which is why each
// part is slugified on its own rather than the joined string.
void CamId__nameOf(Camera* cam) {
  char card[96], serial[64];
  if (!cstr__slugify(card, (u32)sizeof(card), cam->card)) {
    (void)cstr__copy(card, (u32)sizeof(card), "unknown");
  }
  if (!cstr__slugify(serial, (u32)sizeof(serial), cam->serial)) {
    (void)cstr__copy(serial, (u32)sizeof(serial), "unknown");
  }
  (void)snprintf(
      cam->name,
      sizeof(cam->name),
      "%lu__%s__%s",
      (unsigned long)cam->index,
      card,
      serial);
}

// probe one /dev/videoN; false unless it can capture video
static bool _CamId__probe(u32 index, Camera* out) {
  memset(out, 0, sizeof(*out));
  out->index = index;
  (void)snprintf(out->path, sizeof(out->path), "/dev/video%lu", (unsigned long)index);

  int fd = open(out->path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    LOG_DEBUGF("cannot open %s", out->path);
    return false;
  }

  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (0 != ioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    LOG_DEBUGF("QUERYCAP failed on %s", out->path);
    (void)close(fd);
    return false;
  }
  (void)close(fd);

  // device_caps describes THIS node; capabilities describes the whole device
  u32 caps = 0 != (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if (0 == (caps & V4L2_CAP_VIDEO_CAPTURE) || 0 == (caps & V4L2_CAP_STREAMING)) {
    LOG_DEBUGF("%s is not a streaming capture node (caps 0x%08lx)", out->path, (unsigned long)caps);
    return false;
  }

  (void)cstr__copy(out->card, (u32)sizeof(out->card), (const char*)cap.card);
  cstr__trimRight(out->card);
  if (0 == out->card[0]) {
    (void)cstr__copy(out->card, (u32)sizeof(out->card), (const char*)cap.driver);
  }
  (void)_CamId__findSerial(index, out->serial, (u32)sizeof(out->serial));
  CamId__nameOf(out);
  return true;
}

// Enumerate capture-capable cameras, sorted by index
bool CamId__list(CameraList* out) {
  memset(out, 0, sizeof(*out));

  // collect node indices from sysfs (authoritative and cheap)
  u32 indices[MAX_CAMERAS];
  u32 ct = 0;
  DIR* d = opendir("/sys/class/video4linux");
  if (NULL == d) {
    LOG_ERRORF("cannot read /sys/class/video4linux; is V4L2 available?");
    return false;
  }
  struct dirent* ent;
  for (u32 i = 0; i < MAX_LOOP_ITER && ct < MAX_CAMERAS; i++) {
    ent = readdir(d);
    if (NULL == ent) {
      break;
    }
    if (!cstr__eqn(5, ent->d_name, "video")) {
      continue;
    }
    u32 n = 0;
    if (!cstr__toU32(ent->d_name + 5, &n)) {
      continue;
    }
    indices[ct++] = n;
  }
  (void)closedir(d);

  // insertion sort by node index so "camera 0" is stable across reboots
  for (u32 i = 1; i < ct; i++) {
    u32 v = indices[i];
    u32 j = i;
    while (j > 0 && indices[j - 1] > v) {
      indices[j] = indices[j - 1];
      j--;
    }
    indices[j] = v;
  }

  for (u32 i = 0; i < ct && out->ct < MAX_CAMERAS; i++) {
    Camera cam;
    if (_CamId__probe(indices[i], &cam)) {
      out->items[out->ct++] = cam;  // copy
    }
  }
  return true;
}

// Match an id to one camera. Accepts "" / "default" (first camera), a node
// index ("0"), a device path ("/dev/video0"), the full
// "{index}__{card}__{serial}" name, or any case-insensitive substring of it.
bool CamId__resolve(const CameraList* list, const char* id, Camera* out) {
  if (0 == list->ct) {
    return false;
  }
  if (NULL == id || 0 == id[0] || cstr__eq(id, "default")) {
    *out = list->items[0];  // copy
    return true;
  }

  // exact device path
  for (u32 i = 0; i < list->ct; i++) {
    if (cstr__eq(list->items[i].path, id)) {
      *out = list->items[i];
      return true;
    }
  }

  // exact full name
  for (u32 i = 0; i < list->ct; i++) {
    if (cstr__eq(list->items[i].name, id)) {
      *out = list->items[i];
      return true;
    }
  }

  // bare node index
  u32 n = 0;
  if (cstr__toU32(id, &n)) {
    for (u32 i = 0; i < list->ct; i++) {
      if (list->items[i].index == n) {
        *out = list->items[i];
        return true;
      }
    }
    return false;  // a numeric id that matches nothing must not fall through
  }

  // substring of the slug, or of the raw card name so a human-readable
  // fragment like "UVC Camera" still resolves
  for (u32 i = 0; i < list->ct; i++) {
    if (cstr__icontains(list->items[i].name, id) || cstr__icontains(list->items[i].card, id) ||
        cstr__icontains(list->items[i].serial, id)) {
      *out = list->items[i];
      return true;
    }
  }
  return false;
}
