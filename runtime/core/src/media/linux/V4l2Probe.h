/* Copyright (c) ScreenKit contributors. MIT.
 *
 * What video decoding hardware this board has, as the kernel describes it: the
 * V4L2 memory-to-memory decoders, the coded formats each accepts and the
 * largest frame each will decode. Two readers, one answer:
 *
 *   the ScreenKit VLC decoder plugin (vlc-plugin/)  whether to take a stream, and
 *                                                    through which kind of decoder
 *   MediaPlayerLinux.cpp                             the ABR ceiling: the smaller of
 *                                                    the display mode and what the
 *                                                    decoder for the stream's codec
 *                                                    can decode
 *
 * C, because the plugin is a C VLC module. Probing opens each /dev/video* once;
 * callers cache the answer. On a Raspberry Pi 3 (bcm2835-codec) it finds one
 * stateful decoder, /dev/video10: H.264, MPEG-4 part 2, H.263 and MJPEG, up to
 * 1920x1920. */
#ifndef SCREENKIT_MEDIA_V4L2_PROBE_H
#define SCREENKIT_MEDIA_V4L2_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The coded formats this runtime maps to decoders, by V4L2 fourcc. */
enum sk_v4l2_codec {
  SK_V4L2_H264 = 0,
  SK_V4L2_HEVC,
  SK_V4L2_MPEG2,
  SK_V4L2_MPEG4,
  SK_V4L2_VP8,
  SK_V4L2_VP9,
  SK_V4L2_CODEC_COUNT
};

typedef struct sk_v4l2_decoder {
  char device[32];        /* "/dev/video10" */
  char driver[32];        /* "bcm2835-codec" */
  int codec;              /* enum sk_v4l2_codec */
  int stateless;          /* a request-API decoder (H264_SLICE and kin) rather than a stateful M2M one */
  unsigned max_width;     /* the largest frame VIDIOC_ENUM_FRAMESIZES allows; 0 when it says nothing */
  unsigned max_height;
} sk_v4l2_decoder;

/* Every decoder the kernel offers, at most `max` of them. Returns the count. */
int sk_v4l2_probe(sk_v4l2_decoder* out, int max);

/* The best decoder for `codec`: a stateless one when `allow_stateless`, else a
 * stateful one. 0 with `*out` filled, -1 when there is none. */
int sk_v4l2_find(int codec, int allow_stateless, sk_v4l2_decoder* out);

/* "h264", "hevc", ... for logs and for libavcodec's decoder names. */
const char* sk_v4l2_codec_name(int codec);

#ifdef __cplusplus
}
#endif

#endif
