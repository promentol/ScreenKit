/* Copyright (c) ScreenKit contributors. MIT.
 *
 * The V4L2 decoder probe: see V4l2Probe.h. */
#include "V4l2Probe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/videodev2.h>

/* The stateless (request API) formats, which newer kernels name in
 * <linux/videodev2.h>; spelled out so an older header still builds. */
#define SK_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static const struct {
  uint32_t fourcc;
  int codec;
  int stateless;
} kFormats[] = {
    {SK_FOURCC('H', '2', '6', '4'), SK_V4L2_H264, 0},  {SK_FOURCC('S', '2', '6', '4'), SK_V4L2_H264, 1},
    {SK_FOURCC('H', 'E', 'V', 'C'), SK_V4L2_HEVC, 0},  {SK_FOURCC('S', '2', '6', '5'), SK_V4L2_HEVC, 1},
    {SK_FOURCC('M', 'P', 'G', '2'), SK_V4L2_MPEG2, 0}, {SK_FOURCC('M', 'G', '2', 'S'), SK_V4L2_MPEG2, 1},
    {SK_FOURCC('M', 'P', 'G', '4'), SK_V4L2_MPEG4, 0}, {SK_FOURCC('V', 'P', '8', '0'), SK_V4L2_VP8, 0},
    {SK_FOURCC('V', 'P', '8', 'F'), SK_V4L2_VP8, 1},   {SK_FOURCC('V', 'P', '9', '0'), SK_V4L2_VP9, 0},
    {SK_FOURCC('V', 'P', '9', 'F'), SK_V4L2_VP9, 1},
};

static int xioctl(int fd, unsigned long request, void* arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r < 0 && errno == EINTR);
  return r;
}

const char* sk_v4l2_codec_name(int codec) {
  switch (codec) {
    case SK_V4L2_H264: return "h264";
    case SK_V4L2_HEVC: return "hevc";
    case SK_V4L2_MPEG2: return "mpeg2";
    case SK_V4L2_MPEG4: return "mpeg4";
    case SK_V4L2_VP8: return "vp8";
    case SK_V4L2_VP9: return "vp9";
  }
  return "unknown";
}

/* The largest frame the device decodes `fourcc` at. */
static void max_size(int fd, uint32_t fourcc, unsigned* width, unsigned* height) {
  struct v4l2_frmsizeenum size;
  *width = *height = 0;
  for (unsigned i = 0; i < 64; ++i) {
    memset(&size, 0, sizeof(size));
    size.index = i;
    size.pixel_format = fourcc;
    if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
    if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      if (size.discrete.width * size.discrete.height > *width * *height) {
        *width = size.discrete.width;
        *height = size.discrete.height;
      }
    } else {
      *width = size.stepwise.max_width;
      *height = size.stepwise.max_height;
      break;
    }
  }
}

int sk_v4l2_probe(sk_v4l2_decoder* out, int max) {
  int count = 0;
  for (int n = 0; n < 64 && count < max; ++n) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/video%d", n);
    const int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
      close(fd);
      continue;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    const int mplane = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
    if (!mplane && (caps & V4L2_CAP_VIDEO_M2M) == 0) {
      close(fd);
      continue;
    }
    /* A decoder takes coded formats on its OUTPUT queue. */
    const uint32_t type = mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    for (unsigned i = 0; i < 64 && count < max; ++i) {
      struct v4l2_fmtdesc desc;
      memset(&desc, 0, sizeof(desc));
      desc.index = i;
      desc.type = type;
      if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) break;
      if ((desc.flags & V4L2_FMT_FLAG_COMPRESSED) == 0) continue;
      for (size_t f = 0; f < sizeof(kFormats) / sizeof(kFormats[0]); ++f) {
        if (kFormats[f].fourcc != desc.pixelformat) continue;
        sk_v4l2_decoder* d = &out[count++];
        memset(d, 0, sizeof(*d));
        snprintf(d->device, sizeof(d->device), "%s", path);
        snprintf(d->driver, sizeof(d->driver), "%s", (const char*)cap.driver);
        d->codec = kFormats[f].codec;
        d->stateless = kFormats[f].stateless;
        max_size(fd, desc.pixelformat, &d->max_width, &d->max_height);
        break;
      }
    }
    close(fd);
  }
  return count;
}

int sk_v4l2_find(int codec, int allow_stateless, sk_v4l2_decoder* out) {
  sk_v4l2_decoder all[32];
  const int count = sk_v4l2_probe(all, 32);
  int found = -1;
  for (int i = 0; i < count; ++i) {
    if (all[i].codec != codec) continue;
    if (all[i].stateless && !allow_stateless) continue;
    /* Stateless first: it is the one that decodes into buffers of its own. */
    if (found < 0 || (all[i].stateless && !all[found].stateless)) found = i;
  }
  if (found < 0) return -1;
  *out = all[found];
  return 0;
}
