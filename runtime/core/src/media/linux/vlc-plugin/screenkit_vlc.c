/* Copyright (c) ScreenKit contributors. MIT.
 *
 * The ScreenKit VLC plugin (libscreenkit_plugin.so): what the image's VLC 3.0.21
 * lacks for this runtime's <video>, as VLC modules of our own. The player
 * (MediaPlayerLinux.cpp) puts the directory holding it on VLC_PLUGIN_PATH before
 * libvlc_new, so the image's libvlccore loads it beside its own plugins.
 *
 * 1. A video decoder that reaches the hardware. The image's VLC decodes in
 *    software only: its `avcodec` plugin has no hardware path (`--avcodec-hw`
 *    has no module behind it) and its `gstdecode` bridge crashes. This module
 *    outranks avcodec (score 800 against 70), so VLC asks it first, and per
 *    stream it takes, best first:
 *
 *      a stateless V4L2 decoder, where the kernel offers one and the image's
 *        libavcodec has the DRM hwaccel to drive it (the Raspberry Pi FFmpeg
 *        fork's v4l2_request; upstream FFmpeg 4.4 has none, so on Batocera 42
 *        this is never taken);
 *      the stateful V4L2 memory-to-memory decoder for the codec -- a Raspberry
 *        Pi 3's bcm2835-codec -- driven here directly, its decoded frames landing
 *        in capture buffers this module allocates from the kernel's CMA heap and
 *        imports as dma-bufs. That is the point of driving it directly: the CPU
 *        reads those through a cached mapping (4 ms for a 1080p frame on a Pi 3)
 *        where libavcodec's own V4L2 decoder only offers the kernel's uncached
 *        MMAP buffers (35 ms, over the 33 ms a frame has at 30 fps, and memory
 *        traffic that slows the decoder itself);
 *      the same decoder through the image's libavcodec (h264_v4l2m2m and kin),
 *        when there is no CMA heap to allocate from, its frames copied out on
 *        every core at once;
 *      otherwise nothing: the module declines, and VLC's own software `avcodec`
 *        decoder takes the stream as it always did.
 *
 *    Its log -- "ScreenKit decoder: h264 on /dev/video10 (bcm2835-codec,
 *    stateful, direct) ..." -- and VLC's "using video decoder module
 *    \"screenkit\"" say which decoder a stream got. SCREENKIT_VLC_DECODER=software
 *    makes it decline, =lavc skips the direct path; SCREENKIT_VLC_PROFILE=1 logs
 *    where each frame's time goes.
 *
 * 2. An MPEG-TS demuxer for adaptive streaming. VLC's HLS support (`adaptive`)
 *    hands each TS segment to a demuxer it asks for by name, "ts", and the
 *    image's VLC was built without that module (no libdvbpsi) -- so HLS with TS
 *    segments, the most common kind, failed outright. This submodule answers to
 *    "ts" when asked for by name and hands the stream to the image's own
 *    `avformat` demuxer, forced to mpegts. It is never chosen on its own (score
 *    0), so an image that has the real "ts" module keeps using it.
 *
 * 3. What a manifest offers. libvlc says nothing of an HLS or DASH stream's
 *    variants, of whether it is live, or of its protection, and the player needs
 *    all three before the first frame: Shaka lists the variants and the live
 *    window when `load` resolves. This stream filter looks at every stream VLC
 *    opens, and when one is a manifest it reads it -- a peek, so the stream is
 *    untouched -- and logs what it found as lines the player's log callback
 *    parses ("ScreenKit manifest: ...", "ScreenKit variant: ..."). For an HLS
 *    master playlist it also reads the first variant's media playlist, through
 *    VLC, to tell live from VOD. It never takes the stream.
 *
 * Built against the pinned VLC 3.0.21 plugin headers and FFmpeg 4.4.5 headers
 * (tools/batocera/media-headers.sh); links the image's libvlccore, libavcodec
 * and libavutil. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_demux.h>
#include <vlc_modules.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_url.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "../V4l2Probe.h"

static int OpenDecoder(vlc_object_t *);
static void CloseDecoder(vlc_object_t *);
static int OpenTs(vlc_object_t *);
static void CloseTs(vlc_object_t *);
static int OpenManifest(vlc_object_t *);

vlc_module_begin()
    set_shortname("ScreenKit")
    set_description("ScreenKit hardware video decoder (V4L2)")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_capability("video decoder", 800)
    set_callbacks(OpenDecoder, CloseDecoder)
    add_shortcut("screenkit")
    add_submodule()
        set_shortname("ScreenKit TS")
        set_description("MPEG-TS through the image's avformat, for adaptive streaming segments")
        set_category(CAT_INPUT)
        set_subcategory(SUBCAT_INPUT_DEMUX)
        set_capability("demux", 0)
        set_callbacks(OpenTs, CloseTs)
        add_shortcut("ts")
    add_submodule()
        set_shortname("ScreenKit manifest")
        set_description("What an HLS or DASH manifest offers, for the ScreenKit player")
        set_category(CAT_INPUT)
        set_subcategory(SUBCAT_INPUT_STREAM_FILTER)
        set_capability("stream_filter", 40)
        set_callbacks(OpenManifest, NULL)
        add_shortcut("screenkit-manifest")
vlc_module_end()

/* ---- copying frames out, on every core --------------------------------------------- */

typedef struct {
    const uint8_t *src;
    size_t src_pitch;
    uint8_t *dst;
    size_t dst_pitch;
    size_t bytes;
    int rows;
} copy_job;

#define COPY_WORKERS_MAX 7

typedef struct copier {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_cond_t done;
    pthread_t threads[COPY_WORKERS_MAX];
    int workers;
    copy_job jobs[4];
    int job_count;
    unsigned generation;
    int pending;
    int quit;
} copier;

typedef struct {
    copier *c;
    int slice;
} copier_arg;

static void copy_slice(const copier *c, int slice, int slices)
{
    for (int j = 0; j < c->job_count; ++j) {
        const copy_job *job = &c->jobs[j];
        const int first = job->rows * slice / slices;
        const int last = job->rows * (slice + 1) / slices;
        if (job->src_pitch == job->dst_pitch && job->bytes == job->src_pitch) {
            memcpy(job->dst + (size_t)first * job->dst_pitch, job->src + (size_t)first * job->src_pitch,
                   job->bytes * (size_t)(last - first));
            continue;
        }
        for (int row = first; row < last; ++row)
            memcpy(job->dst + (size_t)row * job->dst_pitch, job->src + (size_t)row * job->src_pitch, job->bytes);
    }
}

static void *copier_main(void *data)
{
    copier_arg *arg = data;
    copier *c = arg->c;
    const int slice = arg->slice;
    free(arg);
    unsigned seen = 0;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        while (!c->quit && c->generation == seen)
            pthread_cond_wait(&c->wake, &c->lock);
        if (c->quit)
            break;
        seen = c->generation;
        pthread_mutex_unlock(&c->lock);
        copy_slice(c, slice, c->workers + 1);
        pthread_mutex_lock(&c->lock);
        if (--c->pending == 0)
            pthread_cond_signal(&c->done);
    }
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

static copier *copier_new(int max_workers)
{
    copier *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->wake, NULL);
    pthread_cond_init(&c->done, NULL);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = (int)(cpus > 1 ? cpus - 1 : 0);
    if (workers > max_workers)
        workers = max_workers;
    for (int i = 0; i < workers; ++i) {
        copier_arg *arg = malloc(sizeof(*arg));
        if (arg == NULL)
            break;
        arg->c = c;
        arg->slice = i + 1;
        if (pthread_create(&c->threads[i], NULL, copier_main, arg) != 0) {
            free(arg);
            break;
        }
        c->workers++;
    }
    return c;
}

static void copier_free(copier *c)
{
    if (c == NULL)
        return;
    pthread_mutex_lock(&c->lock);
    c->quit = 1;
    pthread_cond_broadcast(&c->wake);
    pthread_mutex_unlock(&c->lock);
    for (int i = 0; i < c->workers; ++i)
        pthread_join(c->threads[i], NULL);
    pthread_cond_destroy(&c->done);
    pthread_cond_destroy(&c->wake);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

/* Copy every job, the calling thread taking slice 0. */
static void copier_run(copier *c, const copy_job *jobs, int count)
{
    memcpy(c->jobs, jobs, sizeof(*jobs) * (size_t)count);
    c->job_count = count;
    if (c->workers == 0) {
        copy_slice(c, 0, 1);
        return;
    }
    pthread_mutex_lock(&c->lock);
    c->pending = c->workers;
    c->generation++;
    pthread_cond_broadcast(&c->wake);
    pthread_mutex_unlock(&c->lock);
    copy_slice(c, 0, c->workers + 1);
    pthread_mutex_lock(&c->lock);
    while (c->pending > 0)
        pthread_cond_wait(&c->done, &c->lock);
    pthread_mutex_unlock(&c->lock);
}

/* ---- the probe, once per process ---------------------------------------------------- */

static vlc_mutex_t probe_lock = VLC_STATIC_MUTEX;
static int probe_done;
static int probe_count;
static sk_v4l2_decoder probe_result[32];

static int find_hardware(int sk_codec, int want_stateless, sk_v4l2_decoder *out)
{
    vlc_mutex_lock(&probe_lock);
    if (!probe_done) {
        probe_count = sk_v4l2_probe(probe_result, 32);
        probe_done = 1;
    }
    int found = -1;
    for (int i = 0; i < probe_count; ++i) {
        if (probe_result[i].codec == sk_codec && probe_result[i].stateless == want_stateless) {
            found = i;
            break;
        }
    }
    if (found >= 0)
        *out = probe_result[found];
    vlc_mutex_unlock(&probe_lock);
    return found >= 0 ? 0 : -1;
}

/* ---- the decoder's state ---------------------------------------------------------------- */

enum { PATH_DIRECT, PATH_LAVC };

#define OUT_BUFFERS 8
#define CAP_BUFFERS_MAX 32

typedef struct {
    int fd;              /* the device */
    int heap;            /* /dev/dma_heap/linux,cma */
    uint32_t coded;      /* V4L2 fourcc of the bitstream */
    struct {
        void *map;
        size_t length;
        int queued;
    } out[OUT_BUFFERS];
    unsigned out_count;
    size_t out_size;
    struct {
        int dmabuf;
        void *map;
        size_t length;
        int queued;
    } cap[CAP_BUFFERS_MAX];
    unsigned cap_count;
    int cap_streaming;
    uint32_t cap_fourcc;
    unsigned coded_width, coded_height, bytesperline;
    unsigned visible_width, visible_height;
    int eos;             /* drained: the next packet restarts both queues */
    int emitted;         /* pictures out since the open or the last restart */
    int out_streaming;
    AVBSFContext *bsf;   /* avcC/hvcC to Annex B, which a stateful decoder reads */
} direct_state;

struct decoder_sys_t {
    int path;
    sk_v4l2_decoder hw;
    int sk_codec;
    enum AVCodecID codec_id;
    const char *name;        /* what the log calls the path */
    copier *copier;
    mtime_t last_date;
    mtime_t frame_duration;
    uint64_t frames;
    int format_set;
    int profile;
    mtime_t profile_start, spent_wait, spent_copy;

    direct_state d;

    /* PATH_LAVC */
    const AVCodec *codec;
    AVCodecContext *ctx;
    AVFrame *frame;
    AVFrame *download;
    AVPacket *pkt;
    AVBufferRef *hw_device;
    int send_headers;
};

static const struct {
    vlc_fourcc_t vlc;
    int sk;
    enum AVCodecID id;
    const char *lavc;
    uint32_t v4l2;
    const char *bsf;
} kCodecs[] = {
    {VLC_CODEC_H264, SK_V4L2_H264, AV_CODEC_ID_H264, "h264_v4l2m2m", V4L2_PIX_FMT_H264, "h264_mp4toannexb"},
    {VLC_CODEC_HEVC, SK_V4L2_HEVC, AV_CODEC_ID_HEVC, "hevc_v4l2m2m", v4l2_fourcc('H', 'E', 'V', 'C'), "hevc_mp4toannexb"},
    {VLC_CODEC_MPGV, SK_V4L2_MPEG2, AV_CODEC_ID_MPEG2VIDEO, "mpeg2_v4l2m2m", V4L2_PIX_FMT_MPEG2, NULL},
    {VLC_CODEC_MP4V, SK_V4L2_MPEG4, AV_CODEC_ID_MPEG4, "mpeg4_v4l2m2m", V4L2_PIX_FMT_MPEG4, NULL},
    {VLC_CODEC_VP8, SK_V4L2_VP8, AV_CODEC_ID_VP8, "vp8_v4l2m2m", V4L2_PIX_FMT_VP8, NULL},
    {VLC_CODEC_VP9, SK_V4L2_VP9, AV_CODEC_ID_VP9, "vp9_v4l2m2m", v4l2_fourcc('V', 'P', '9', '0'), NULL},
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int annexb(const uint8_t *p, size_t n)
{
    return p != NULL && n >= 4 && ((p[0] == 0 && p[1] == 0 && p[2] == 1) || (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1));
}

/* ---- handing a frame to VLC ------------------------------------------------------------ */

typedef struct {
    int width, height;
    vlc_fourcc_t chroma;
    const uint8_t *planes[3];
    size_t pitches[3];
    int sar_num, sar_den;
    int progressive;
} frame_view;

static int queue_frame(decoder_t *dec, const frame_view *f, int64_t pts)
{
    decoder_sys_t *sys = dec->p_sys;
    video_format_t *out = &dec->fmt_out.video;
    /* The first frame always: decoder_NewPicture needs a format the vout was told. */
    if (!sys->format_set || dec->fmt_out.i_codec != f->chroma || out->i_width != (unsigned)f->width ||
        out->i_height != (unsigned)f->height) {
        dec->fmt_out.i_codec = f->chroma;
        out->i_chroma = f->chroma;
        out->i_width = out->i_visible_width = (unsigned)f->width;
        out->i_height = out->i_visible_height = (unsigned)f->height;
        out->i_x_offset = out->i_y_offset = 0;
        if (f->sar_num > 0 && f->sar_den > 0) {
            out->i_sar_num = (unsigned)f->sar_num;
            out->i_sar_den = (unsigned)f->sar_den;
        } else if (out->i_sar_num == 0 || out->i_sar_den == 0) {
            out->i_sar_num = out->i_sar_den = 1;
        }
        if (decoder_UpdateVideoFormat(dec))
            return VLC_EGENERIC;
        sys->format_set = 1;
    }
    picture_t *pic = decoder_NewPicture(dec);
    if (pic == NULL)
        return VLC_EGENERIC;
    const mtime_t copy_start = sys->profile ? mdate() : 0;
    copy_job jobs[4];
    int count = 0;
    for (int p = 0; p < pic->i_planes && p < 3; ++p) {
        if (f->planes[p] == NULL)
            break;
        copy_job *job = &jobs[count++];
        job->src = f->planes[p];
        job->src_pitch = f->pitches[p];
        job->dst = pic->p[p].p_pixels;
        job->dst_pitch = (size_t)pic->p[p].i_pitch;
        job->bytes = __MIN((size_t)pic->p[p].i_visible_pitch, f->pitches[p]);
        job->rows = pic->p[p].i_visible_lines;
    }
    copier_run(sys->copier, jobs, count);
    if (pts <= VLC_TS_INVALID)
        pts = sys->last_date > VLC_TS_INVALID ? sys->last_date + sys->frame_duration : VLC_TS_INVALID;
    pic->date = pts;
    pic->b_progressive = f->progressive;
    pic->i_nb_fields = 2;
    if (pts > VLC_TS_INVALID)
        sys->last_date = pts;
    if (sys->frames == 0) {
        sys->profile_start = mdate();
        msg_Info(dec, "ScreenKit decoder: first %dx%d %4.4s frame from %s", f->width, f->height,
                 (const char *)&f->chroma, sys->name);
    }
    sys->frames++;
    if (sys->profile) {
        const mtime_t now = mdate();
        sys->spent_copy += now - copy_start;
        if (sys->frames % 60 == 0)
            msg_Info(dec, "ScreenKit decoder profile: %" PRIu64 " frames, %.2f ms/frame waiting on the decoder, "
                     "%.2f ms/frame copying, %.1f frames/s, picture %" PRId64 " ms from now",
                     sys->frames, sys->spent_wait / 1000.0 / sys->frames, sys->spent_copy / 1000.0 / sys->frames,
                     sys->frames * 1e6 / (double)(now - sys->profile_start + 1),
                     (decoder_GetDisplayDate(dec, pic->date) - now) / 1000);
    }
    decoder_QueueVideo(dec, pic);
    return VLC_SUCCESS;
}

/* ---- the direct path: a stateful V4L2 decoder, capture buffers from the CMA heap -------- */

static void direct_free_capture(direct_state *d)
{
    for (unsigned i = 0; i < d->cap_count; ++i) {
        if (d->cap[i].map != NULL && d->cap[i].map != MAP_FAILED)
            munmap(d->cap[i].map, d->cap[i].length);
        if (d->cap[i].dmabuf >= 0)
            close(d->cap[i].dmabuf);
        d->cap[i].map = NULL;
        d->cap[i].dmabuf = -1;
        d->cap[i].queued = 0;
    }
    d->cap_count = 0;
}

static int direct_queue_capture(direct_state *d, unsigned i)
{
    struct v4l2_plane plane;
    struct v4l2_buffer buf;
    memset(&plane, 0, sizeof(plane));
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = i;
    buf.length = 1;
    buf.m.planes = &plane;
    plane.m.fd = d->cap[i].dmabuf;
    plane.length = (uint32_t)d->cap[i].length;
    if (xioctl(d->fd, VIDIOC_QBUF, &buf) < 0)
        return -1;
    d->cap[i].queued = 1;
    return 0;
}

static void direct_stop_capture(direct_state *d)
{
    if (d->cap_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(d->fd, VIDIOC_STREAMOFF, &type);
        d->cap_streaming = 0;
    }
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    xioctl(d->fd, VIDIOC_REQBUFS, &req);
    direct_free_capture(d);
}

/* The decoder has parsed the stream's headers: learn the picture and give it
 * somewhere to decode into. */
static int direct_setup_capture(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    direct_stop_capture(d);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_G_FMT, &fmt) < 0)
        return -1;
    /* YU12 is what the Wayland plane and VLC's I420 share; NV12 otherwise. */
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420) {
        struct v4l2_format want = fmt;
        want.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
        if (xioctl(d->fd, VIDIOC_S_FMT, &want) == 0 && want.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUV420)
            fmt = want;
    }
    if (fmt.fmt.pix_mp.num_planes != 1 ||
        (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 && fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12)) {
        msg_Err(dec, "ScreenKit decoder: the decoder's %4.4s in %u planes is not a layout this module reads",
                (const char *)&fmt.fmt.pix_mp.pixelformat, fmt.fmt.pix_mp.num_planes);
        return -1;
    }
    d->cap_fourcc = fmt.fmt.pix_mp.pixelformat;
    d->coded_width = fmt.fmt.pix_mp.width;
    d->coded_height = fmt.fmt.pix_mp.height;
    d->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    const size_t size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    if (xioctl(d->fd, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width > 0 && sel.r.height > 0) {
        d->visible_width = sel.r.width;
        d->visible_height = sel.r.height;
    } else {
        d->visible_width = d->coded_width;
        d->visible_height = d->coded_height;
    }

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    unsigned count = 6;
    if (xioctl(d->fd, VIDIOC_G_CTRL, &ctrl) == 0 && ctrl.value > 0)
        count = (unsigned)ctrl.value + 4;
    if (count > CAP_BUFFERS_MAX)
        count = CAP_BUFFERS_MAX;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    req.count = count;
    if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0)
        return -1;
    for (unsigned i = 0; i < req.count; ++i) {
        struct dma_heap_allocation_data alloc;
        memset(&alloc, 0, sizeof(alloc));
        alloc.len = size;
        alloc.fd_flags = O_RDWR | O_CLOEXEC;
        d->cap[i].dmabuf = -1;
        d->cap[i].map = NULL;
        if (xioctl(d->heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
            msg_Err(dec, "ScreenKit decoder: the CMA heap has no %zu bytes for a frame", size);
            d->cap_count = i;
            direct_stop_capture(d);
            return -1;
        }
        d->cap[i].dmabuf = (int)alloc.fd;
        d->cap[i].length = size;
        d->cap[i].map = mmap(NULL, size, PROT_READ, MAP_SHARED, (int)alloc.fd, 0);
        d->cap_count = i + 1;
        if (d->cap[i].map == MAP_FAILED || direct_queue_capture(d, i) < 0) {
            msg_Err(dec, "ScreenKit decoder: could not map or queue a capture buffer: %s", strerror(errno));
            direct_stop_capture(d);
            return -1;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0) {
        direct_stop_capture(d);
        return -1;
    }
    d->cap_streaming = 1;
    msg_Dbg(dec, "ScreenKit decoder: %ux%u (coded %ux%u) %4.4s, %u capture buffers of %zu bytes from the CMA heap",
            d->visible_width, d->visible_height, d->coded_width, d->coded_height, (const char *)&d->cap_fourcc,
            d->cap_count, size);
    return 0;
}

/* One decoded frame out of capture buffer `i`, into VLC, and the buffer back. */
static int direct_emit(decoder_t *dec, unsigned i, const struct v4l2_buffer *buf)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    xioctl(d->cap[i].dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
    const uint8_t *base = d->cap[i].map;
    frame_view f;
    memset(&f, 0, sizeof(f));
    f.width = (int)d->visible_width;
    f.height = (int)d->visible_height;
    f.progressive = buf->field != V4L2_FIELD_INTERLACED;
    const size_t luma = (size_t)d->bytesperline * d->coded_height;
    f.planes[0] = base;
    f.pitches[0] = d->bytesperline;
    if (d->cap_fourcc == V4L2_PIX_FMT_YUV420) {
        f.chroma = VLC_CODEC_I420;
        f.planes[1] = base + luma;
        f.pitches[1] = d->bytesperline / 2;
        f.planes[2] = base + luma + (size_t)(d->bytesperline / 2) * ((d->coded_height + 1) / 2);
        f.pitches[2] = d->bytesperline / 2;
    } else {
        f.chroma = VLC_CODEC_NV12;
        f.planes[1] = base + luma;
        f.pitches[1] = d->bytesperline;
    }
    const int64_t pts = (int64_t)buf->timestamp.tv_sec * CLOCK_FREQ + buf->timestamp.tv_usec;
    const int ret = queue_frame(dec, &f, pts);
    d->emitted++;
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    xioctl(d->cap[i].dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
    return ret;
}

/* Does the decoder's capture format still match the buffers capture has? */
static int direct_capture_stale(direct_state *d)
{
    if (!d->cap_streaming)
        return 1;
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_G_FMT, &fmt) < 0)
        return 1;
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    if (xioctl(d->fd, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width > 0 && sel.r.height > 0) {
        d->visible_width = sel.r.width;
        d->visible_height = sel.r.height;
    }
    return fmt.fmt.pix_mp.width != d->coded_width || fmt.fmt.pix_mp.height != d->coded_height ||
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline != d->bytesperline ||
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage > (d->cap_count > 0 ? d->cap[0].length : 0);
}

/* Frames already decoded at the old size, out before capture is rebuilt. */
static void direct_take_ready(decoder_t *dec)
{
    direct_state *d = &dec->p_sys->d;
    while (d->cap_streaming) {
        struct v4l2_plane plane;
        struct v4l2_buffer buf;
        memset(&plane, 0, sizeof(plane));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.length = 1;
        buf.m.planes = &plane;
        if (xioctl(d->fd, VIDIOC_DQBUF, &buf) < 0 || buf.index >= d->cap_count)
            break;
        d->cap[buf.index].queued = 0;
        if (plane.bytesused > 0 && !(buf.flags & V4L2_BUF_FLAG_ERROR))
            direct_emit(dec, buf.index, &buf);
    }
}

/* Serve the device: events, finished bitstream buffers, decoded frames. Waits up
 * to `timeout` ms for something to happen; returns -1 on a device failure. */
static int direct_service(decoder_t *dec, int timeout)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    struct pollfd pfd = {.fd = d->fd, .events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLPRI};
    const mtime_t start = sys->profile ? mdate() : 0;
    int r = poll(&pfd, 1, timeout);
    if (sys->profile)
        sys->spent_wait += mdate() - start;
    if (r <= 0)
        return 0;
    if (pfd.revents & POLLPRI) {
        struct v4l2_event ev;
        memset(&ev, 0, sizeof(ev));
        while (xioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0) {
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                /* The stream's own picture, now the decoder has read its headers
                 * -- at the start, and at a new resolution mid-stream (an adaptive
                 * switch). Capture is rebuilt only if it no longer fits. */
                if (direct_capture_stale(d)) {
                    direct_take_ready(dec);
                    if (direct_setup_capture(dec) < 0)
                        return -1;
                }
            } else if (ev.type == V4L2_EVENT_EOS) {
                d->eos = 1;
            }
            memset(&ev, 0, sizeof(ev));
        }
    }
    /* Finished bitstream buffers. */
    for (;;) {
        struct v4l2_plane plane;
        struct v4l2_buffer buf;
        memset(&plane, 0, sizeof(plane));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = &plane;
        if (xioctl(d->fd, VIDIOC_DQBUF, &buf) < 0)
            break;
        if (buf.index < d->out_count)
            d->out[buf.index].queued = 0;
    }
    /* Decoded frames. */
    while (d->cap_streaming) {
        struct v4l2_plane plane;
        struct v4l2_buffer buf;
        memset(&plane, 0, sizeof(plane));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.length = 1;
        buf.m.planes = &plane;
        if (xioctl(d->fd, VIDIOC_DQBUF, &buf) < 0)
            break;
        if (buf.index >= d->cap_count)
            continue;
        d->cap[buf.index].queued = 0;
        const int last = (buf.flags & V4L2_BUF_FLAG_LAST) != 0;
        if (plane.bytesused > 0 && !(buf.flags & V4L2_BUF_FLAG_ERROR))
            direct_emit(dec, buf.index, &buf);
        if (last) {
            d->eos = 1;
            break;
        }
        direct_queue_capture(d, buf.index);
    }
    return 0;
}

static int direct_start_output(direct_state *d)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0)
        return -1;
    d->out_streaming = 1;
    return 0;
}

/* Back to a clean start on both queues, keeping the capture buffers: after a
 * seek, and after a drain. */
static void direct_restart(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(d->fd, VIDIOC_STREAMOFF, &type);
    d->out_streaming = 0;
    for (unsigned i = 0; i < d->out_count; ++i)
        d->out[i].queued = 0;
    if (d->cap_streaming) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(d->fd, VIDIOC_STREAMOFF, &type);
        for (unsigned i = 0; i < d->cap_count; ++i)
            d->cap[i].queued = 0;
        for (unsigned i = 0; i < d->cap_count; ++i)
            direct_queue_capture(d, i);
        xioctl(d->fd, VIDIOC_STREAMON, &type);
    }
    struct v4l2_decoder_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = V4L2_DEC_CMD_START;
    xioctl(d->fd, VIDIOC_DECODER_CMD, &cmd);
    direct_start_output(d);
    if (d->bsf != NULL)
        av_bsf_flush(d->bsf);
    d->eos = 0;
    d->emitted = 0;
    sys->send_headers = annexb(dec->fmt_in.p_extra, dec->fmt_in.i_extra);
}

/* Until the first picture after an open or a seek is out, wait up to `ms` for
 * it. VLC prerolls -- buffers, then waits for each decoder's first picture --
 * before the clock starts; a decoder that only hands pictures over when fed
 * would give its first one after playback began, and the video output (made
 * for that first picture) would start behind the clock, dropping frames to
 * catch up. Only the decoded-frame queue is waited on: a free bitstream buffer
 * is no reason to return. */
static int direct_wait_first(decoder_t *dec, int ms)
{
    direct_state *d = &dec->p_sys->d;
    const mtime_t deadline = mdate() + (mtime_t)ms * 1000;
    while (d->emitted == 0 && d->cap_streaming) {
        const mtime_t left = deadline - mdate();
        if (left <= 0)
            break;
        struct pollfd pfd = {.fd = d->fd, .events = POLLIN | POLLRDNORM | POLLPRI};
        if (poll(&pfd, 1, (int)(left / 1000) + 1) <= 0)
            break;
        if (direct_service(dec, 0) < 0)
            return -1;
    }
    return 0;
}

/* One access unit into a free bitstream buffer, waiting for one if need be. */
static int direct_queue_bitstream(decoder_t *dec, const uint8_t *data, size_t size, int64_t pts)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    const size_t headers = sys->send_headers ? (size_t)dec->fmt_in.i_extra : 0;
    if (headers + size > d->out_size) {
        msg_Warn(dec, "ScreenKit decoder: a %zu-byte access unit is larger than the decoder's %zu-byte buffers; dropped",
                 headers + size, d->out_size);
        return 0;
    }
    for (int tries = 0; tries < 200; ++tries) {
        for (unsigned i = 0; i < d->out_count; ++i) {
            if (d->out[i].queued)
                continue;
            uint8_t *to = d->out[i].map;
            if (headers > 0)
                memcpy(to, dec->fmt_in.p_extra, headers);
            memcpy(to + headers, data, size);
            sys->send_headers = 0;
            struct v4l2_plane plane;
            struct v4l2_buffer buf;
            memset(&plane, 0, sizeof(plane));
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            buf.length = 1;
            buf.m.planes = &plane;
            plane.bytesused = (uint32_t)(headers + size);
            plane.length = (uint32_t)d->out[i].length;
            if (pts > VLC_TS_INVALID) {
                buf.timestamp.tv_sec = pts / CLOCK_FREQ;
                buf.timestamp.tv_usec = pts % CLOCK_FREQ;
            }
            if (xioctl(d->fd, VIDIOC_QBUF, &buf) < 0) {
                msg_Err(dec, "ScreenKit decoder: queueing a bitstream buffer failed: %s", strerror(errno));
                return -1;
            }
            d->out[i].queued = 1;
            return 0;
        }
        /* Every bitstream buffer is with the decoder: take its frames until one frees. */
        if (direct_service(dec, 50) < 0)
            return -1;
    }
    msg_Err(dec, "ScreenKit decoder: the decoder stopped taking bitstream");
    return -1;
}

static int direct_open(decoder_t *dec, const char *bsf_name)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    d->fd = -1;
    d->heap = open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
    if (d->heap < 0)
        return -1;
    d->fd = open(sys->hw.device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0)
        return -1;
    const unsigned width = dec->fmt_in.video.i_visible_width ? dec->fmt_in.video.i_visible_width : dec->fmt_in.video.i_width;
    const unsigned height = dec->fmt_in.video.i_visible_height ? dec->fmt_in.video.i_visible_height : dec->fmt_in.video.i_height;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = d->coded;
    fmt.fmt.pix_mp.width = width ? width : 1920;
    fmt.fmt.pix_mp.height = height ? height : 1080;
    fmt.fmt.pix_mp.num_planes = 1;
    /* Room for the largest access unit a stream of this size plausibly has. */
    size_t want = (size_t)fmt.fmt.pix_mp.width * fmt.fmt.pix_mp.height;
    if (want < (1u << 20))
        want = 1u << 20;
    if (want > (4u << 20))
        want = 4u << 20;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (uint32_t)want;
    if (xioctl(d->fd, VIDIOC_S_FMT, &fmt) < 0)
        return -1;

    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
        return -1;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_EOS;
    xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = OUT_BUFFERS;
    if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0)
        return -1;
    d->out_count = req.count > OUT_BUFFERS ? OUT_BUFFERS : req.count;
    for (unsigned i = 0; i < d->out_count; ++i) {
        struct v4l2_plane plane;
        struct v4l2_buffer buf;
        memset(&plane, 0, sizeof(plane));
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = &plane;
        if (xioctl(d->fd, VIDIOC_QUERYBUF, &buf) < 0)
            return -1;
        d->out[i].length = plane.length;
        d->out[i].map = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, plane.m.mem_offset);
        if (d->out[i].map == MAP_FAILED) {
            d->out[i].map = NULL;
            return -1;
        }
        if (d->out_size == 0 || plane.length < d->out_size)
            d->out_size = plane.length;
    }
    if (direct_start_output(d) < 0)
        return -1;
    /* Capture streams from the start, in the format the decoder proposes for
     * this bitstream: the decoder consumes nothing until it does. The stream's
     * own headers then correct it through SOURCE_CHANGE if they must. */
    if (direct_setup_capture(dec) < 0)
        return -1;

    /* avcC and hvcC become Annex B here, parameter sets in-band: a stateful
     * decoder reads a byte stream and never sees VLC's extradata. */
    if (bsf_name != NULL && dec->fmt_in.i_extra > 0 && !annexb(dec->fmt_in.p_extra, dec->fmt_in.i_extra)) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name(bsf_name);
        if (filter == NULL || av_bsf_alloc(filter, &d->bsf) < 0)
            return -1;
        d->bsf->par_in->codec_id = sys->codec_id;
        d->bsf->par_in->extradata = av_mallocz(dec->fmt_in.i_extra + AV_INPUT_BUFFER_PADDING_SIZE);
        if (d->bsf->par_in->extradata == NULL)
            return -1;
        memcpy(d->bsf->par_in->extradata, dec->fmt_in.p_extra, dec->fmt_in.i_extra);
        d->bsf->par_in->extradata_size = dec->fmt_in.i_extra;
        if (av_bsf_init(d->bsf) < 0)
            return -1;
    }
    sys->pkt = av_packet_alloc();
    if (sys->pkt == NULL)
        return -1;
    sys->send_headers = annexb(dec->fmt_in.p_extra, dec->fmt_in.i_extra);
    return 0;
}

static void direct_close(decoder_sys_t *sys)
{
    direct_state *d = &sys->d;
    if (d->fd >= 0) {
        direct_stop_capture(d);
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(d->fd, VIDIOC_STREAMOFF, &type);
        for (unsigned i = 0; i < d->out_count; ++i) {
            if (d->out[i].map != NULL)
                munmap(d->out[i].map, d->out[i].length);
        }
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(d->fd, VIDIOC_REQBUFS, &req);
        close(d->fd);
    }
    if (d->heap >= 0)
        close(d->heap);
    av_bsf_free(&d->bsf);
    av_packet_free(&sys->pkt);
    d->fd = d->heap = -1;
}

static int direct_decode(decoder_t *dec, block_t *block)
{
    decoder_sys_t *sys = dec->p_sys;
    direct_state *d = &sys->d;
    if (block == NULL) {
        /* Drain: ask for everything the decoder holds, until its LAST buffer. */
        struct v4l2_decoder_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = V4L2_DEC_CMD_STOP;
        if (d->cap_streaming && xioctl(d->fd, VIDIOC_DECODER_CMD, &cmd) == 0) {
            const mtime_t deadline = mdate() + 2 * CLOCK_FREQ;
            while (!d->eos && mdate() < deadline) {
                if (direct_service(dec, 50) < 0)
                    break;
            }
        }
        d->eos = 1;
        return VLCDEC_SUCCESS;
    }
    if (block->i_flags & BLOCK_FLAG_CORRUPTED) {
        block_Release(block);
        return VLCDEC_SUCCESS;
    }
    if (d->eos)
        direct_restart(dec);
    const int64_t pts = block->i_pts > VLC_TS_INVALID ? block->i_pts : block->i_dts;
    if (block->i_length > 0)
        sys->frame_duration = block->i_length;
    int ret = 0;
    if (d->bsf != NULL) {
        if (av_new_packet(sys->pkt, (int)block->i_buffer) == 0) {
            memcpy(sys->pkt->data, block->p_buffer, block->i_buffer);
            if (av_bsf_send_packet(d->bsf, sys->pkt) == 0) {
                while (ret == 0 && av_bsf_receive_packet(d->bsf, sys->pkt) == 0) {
                    ret = direct_queue_bitstream(dec, sys->pkt->data, (size_t)sys->pkt->size, pts);
                    av_packet_unref(sys->pkt);
                }
            }
            av_packet_unref(sys->pkt);
        }
    } else {
        ret = direct_queue_bitstream(dec, block->p_buffer, block->i_buffer, pts);
    }
    block_Release(block);
    if (ret < 0)
        return VLCDEC_ECRITICAL;
    /* Whatever the decoder has ready, without waiting for more -- except for the
     * first picture, which VLC's preroll waits on. */
    if (direct_service(dec, 0) < 0)
        return VLCDEC_ECRITICAL;
    if (d->emitted == 0 && direct_wait_first(dec, 40) < 0)
        return VLCDEC_ECRITICAL;
    return VLCDEC_SUCCESS;
}

/* ---- the libavcodec path -------------------------------------------------------------- */

/* Does the image's libavcodec drive stateless V4L2 for this codec? Its software
 * decoder then offers a DRM hwaccel. */
static const AVCodec *stateless_decoder(enum AVCodecID id)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    if (codec == NULL)
        return NULL;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (config == NULL)
            return NULL;
        if (config->device_type == AV_HWDEVICE_TYPE_DRM && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return codec;
    }
}

static enum AVPixelFormat pick_drm(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
    (void)ctx;
    for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_DRM_PRIME)
            return *f;
    }
    return formats[0];
}

static int lavc_open(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;
    sys->ctx = avcodec_alloc_context3(sys->codec);
    if (sys->ctx == NULL)
        return VLC_ENOMEM;
    AVCodecContext *ctx = sys->ctx;
    /* VLC's clock is microseconds, and the V4L2 decoders carry a timestamp
     * through the hardware in the codec's time base: keep them the same. */
    ctx->time_base = (AVRational){1, CLOCK_FREQ};
    ctx->pkt_timebase = (AVRational){1, CLOCK_FREQ};
    ctx->codec_id = sys->codec_id;
    ctx->width = dec->fmt_in.video.i_visible_width ? dec->fmt_in.video.i_visible_width : dec->fmt_in.video.i_width;
    ctx->height = dec->fmt_in.video.i_visible_height ? dec->fmt_in.video.i_visible_height : dec->fmt_in.video.i_height;
    if (dec->fmt_in.i_extra > 0 && dec->fmt_in.p_extra != NULL) {
        ctx->extradata = av_mallocz(dec->fmt_in.i_extra + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ctx->extradata == NULL) {
            avcodec_free_context(&sys->ctx);
            return VLC_ENOMEM;
        }
        memcpy(ctx->extradata, dec->fmt_in.p_extra, dec->fmt_in.i_extra);
        ctx->extradata_size = dec->fmt_in.i_extra;
    }
    if (sys->hw_device != NULL) {
        ctx->hw_device_ctx = av_buffer_ref(sys->hw_device);
        ctx->get_format = pick_drm;
    }
    if (avcodec_open2(ctx, sys->codec, NULL) < 0) {
        avcodec_free_context(&sys->ctx);
        return VLC_EGENERIC;
    }
    /* The V4L2 decoders never send extradata to the device: Annex B parameter
     * sets go in-band with the first packet. (avcC's the codec's own filter does.) */
    sys->send_headers = annexb(dec->fmt_in.p_extra, dec->fmt_in.i_extra);
    return VLC_SUCCESS;
}

static int lavc_emit(decoder_t *dec, AVFrame *frame)
{
    decoder_sys_t *sys = dec->p_sys;
    AVFrame *src = frame;
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        av_frame_unref(sys->download);
        if (av_hwframe_transfer_data(sys->download, frame, 0) < 0) {
            msg_Err(dec, "could not download a stateless V4L2 frame");
            return VLC_EGENERIC;
        }
        sys->download->pts = frame->pts;
        sys->download->best_effort_timestamp = frame->best_effort_timestamp;
        sys->download->sample_aspect_ratio = frame->sample_aspect_ratio;
        src = sys->download;
    }
    frame_view f;
    memset(&f, 0, sizeof(f));
    switch (src->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            f.chroma = VLC_CODEC_I420;
            break;
        case AV_PIX_FMT_NV12:
            f.chroma = VLC_CODEC_NV12;
            break;
        default:
            msg_Err(dec, "the V4L2 decoder produced %s, which this module does not hand on",
                    av_get_pix_fmt_name(src->format));
            return VLC_EGENERIC;
    }
    f.width = src->width;
    f.height = src->height;
    for (int p = 0; p < 3; ++p) {
        f.planes[p] = src->data[p];
        f.pitches[p] = (size_t)abs(src->linesize[p]);
    }
    f.sar_num = src->sample_aspect_ratio.num;
    f.sar_den = src->sample_aspect_ratio.den;
    f.progressive = !src->interlaced_frame;
    int64_t pts = src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts;
    if (pts == AV_NOPTS_VALUE)
        pts = VLC_TS_INVALID;
    return queue_frame(dec, &f, pts);
}

static int lavc_receive(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;
    for (;;) {
        const mtime_t start = sys->profile ? mdate() : 0;
        int ret = avcodec_receive_frame(sys->ctx, sys->frame);
        if (sys->profile)
            sys->spent_wait += mdate() - start;
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return VLC_SUCCESS;
        if (ret < 0) {
            char why[64];
            av_strerror(ret, why, sizeof(why));
            msg_Warn(dec, "the V4L2 decoder failed a frame: %s", why);
            return VLC_EGENERIC;
        }
        ret = lavc_emit(dec, sys->frame);
        av_frame_unref(sys->frame);
        if (ret != VLC_SUCCESS)
            return ret;
    }
}

static int lavc_decode(decoder_t *dec, block_t *block)
{
    decoder_sys_t *sys = dec->p_sys;
    if (sys->ctx == NULL) {
        if (block != NULL)
            block_Release(block);
        return VLCDEC_ECRITICAL;
    }
    if (block == NULL) {
        avcodec_send_packet(sys->ctx, NULL);
        lavc_receive(dec);
        return VLCDEC_SUCCESS;
    }
    if (block->i_flags & BLOCK_FLAG_CORRUPTED) {
        block_Release(block);
        return VLCDEC_SUCCESS;
    }
    const size_t headers = sys->send_headers ? (size_t)dec->fmt_in.i_extra : 0;
    if (av_new_packet(sys->pkt, (int)(headers + block->i_buffer)) < 0) {
        block_Release(block);
        return VLCDEC_ECRITICAL;
    }
    if (headers > 0)
        memcpy(sys->pkt->data, dec->fmt_in.p_extra, headers);
    memcpy(sys->pkt->data + headers, block->p_buffer, block->i_buffer);
    sys->send_headers = 0;
    sys->pkt->pts = block->i_pts > VLC_TS_INVALID ? block->i_pts : AV_NOPTS_VALUE;
    sys->pkt->dts = block->i_dts > VLC_TS_INVALID ? block->i_dts : AV_NOPTS_VALUE;
    if (sys->pkt->pts == AV_NOPTS_VALUE)
        sys->pkt->pts = sys->pkt->dts;
    if (block->i_length > 0)
        sys->frame_duration = block->i_length;
    block_Release(block);
    for (int tries = 0; tries < 100; ++tries) {
        const mtime_t start = sys->profile ? mdate() : 0;
        int ret = avcodec_send_packet(sys->ctx, sys->pkt);
        if (sys->profile)
            sys->spent_wait += mdate() - start;
        if (ret != AVERROR(EAGAIN))
            break;
        /* The decoder is full: take what it has, then give it the packet. */
        if (lavc_receive(dec) != VLC_SUCCESS)
            break;
    }
    av_packet_unref(sys->pkt);
    lavc_receive(dec);
    return VLCDEC_SUCCESS;
}

/* ---- the module ----------------------------------------------------------------------- */

static int Decode(decoder_t *dec, block_t *block)
{
    decoder_sys_t *sys = dec->p_sys;
    return sys->path == PATH_DIRECT ? direct_decode(dec, block) : lavc_decode(dec, block);
}

/* After a seek: nothing from before it may come out. */
static void Flush(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;
    if (sys->path == PATH_DIRECT) {
        direct_restart(dec);
    } else {
        /* A fresh context is the one reset every V4L2 decoder in libavcodec honours. */
        avcodec_free_context(&sys->ctx);
        if (lavc_open(dec) != VLC_SUCCESS)
            msg_Err(dec, "could not reopen %s after a flush", sys->name);
    }
    sys->last_date = VLC_TS_INVALID;
}

static void release(decoder_sys_t *sys)
{
    if (sys->path == PATH_DIRECT) {
        direct_close(sys);
    } else {
        avcodec_free_context(&sys->ctx);
        av_frame_free(&sys->frame);
        av_frame_free(&sys->download);
        av_packet_free(&sys->pkt);
        av_buffer_unref(&sys->hw_device);
    }
    copier_free(sys->copier);
    free(sys);
}

static int OpenDecoder(vlc_object_t *obj)
{
    decoder_t *dec = (decoder_t *)obj;
    if (dec->fmt_in.i_cat != VIDEO_ES)
        return VLC_EGENERIC;
    const char *mode = getenv("SCREENKIT_VLC_DECODER");
    if (mode != NULL && strcmp(mode, "software") == 0)
        return VLC_EGENERIC;
    size_t which = 0;
    for (; which < sizeof(kCodecs) / sizeof(kCodecs[0]); ++which) {
        if (kCodecs[which].vlc == dec->fmt_in.i_codec)
            break;
    }
    if (which == sizeof(kCodecs) / sizeof(kCodecs[0]))
        return VLC_EGENERIC;

    decoder_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;
    sys->sk_codec = kCodecs[which].sk;
    sys->codec_id = kCodecs[which].id;
    sys->d.fd = sys->d.heap = -1;
    sys->d.coded = kCodecs[which].v4l2;
    sys->profile = getenv("SCREENKIT_VLC_PROFILE") != NULL;
    sys->frame_duration = CLOCK_FREQ / 25;
    if (dec->fmt_in.video.i_frame_rate > 0 && dec->fmt_in.video.i_frame_rate_base > 0)
        sys->frame_duration = CLOCK_FREQ * dec->fmt_in.video.i_frame_rate_base / dec->fmt_in.video.i_frame_rate;
    dec->p_sys = sys;

    const unsigned width = dec->fmt_in.video.i_visible_width ? dec->fmt_in.video.i_visible_width : dec->fmt_in.video.i_width;
    const unsigned height = dec->fmt_in.video.i_visible_height ? dec->fmt_in.video.i_visible_height : dec->fmt_in.video.i_height;
    int opened = 0;

    /* 1. Stateless, through the image's libavcodec, where both sides have it. */
    if (find_hardware(sys->sk_codec, 1, &sys->hw) == 0 && (sys->codec = stateless_decoder(sys->codec_id)) != NULL &&
        av_hwdevice_ctx_create(&sys->hw_device, AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0) == 0) {
        sys->path = PATH_LAVC;
        sys->name = sys->codec->name;
        sys->frame = av_frame_alloc();
        sys->download = av_frame_alloc();
        sys->pkt = av_packet_alloc();
        opened = sys->frame && sys->download && sys->pkt && lavc_open(dec) == VLC_SUCCESS;
        if (!opened) {
            avcodec_free_context(&sys->ctx);
            av_frame_free(&sys->frame);
            av_frame_free(&sys->download);
            av_packet_free(&sys->pkt);
            av_buffer_unref(&sys->hw_device);
        }
    }
    /* 2. and 3. Stateful: directly, else through libavcodec. */
    if (!opened && find_hardware(sys->sk_codec, 0, &sys->hw) == 0) {
        if (sys->hw.max_width > 0 && sys->hw.max_height > 0 && (width > sys->hw.max_width || height > sys->hw.max_height)) {
            msg_Dbg(dec, "ScreenKit decoder: %ux%u is beyond %s (%ux%u); software decodes it", width, height,
                    sys->hw.device, sys->hw.max_width, sys->hw.max_height);
            free(sys);
            dec->p_sys = NULL;
            return VLC_EGENERIC;
        }
        if (mode == NULL || strcmp(mode, "lavc") != 0) {
            sys->path = PATH_DIRECT;
            sys->name = "direct";
            opened = direct_open(dec, kCodecs[which].bsf) == 0;
            if (!opened) {
                msg_Dbg(dec, "ScreenKit decoder: driving %s directly failed (%s); trying libavcodec", sys->hw.device,
                        strerror(errno));
                direct_close(sys);
            }
        }
        if (!opened && (sys->codec = avcodec_find_decoder_by_name(kCodecs[which].lavc)) != NULL) {
            sys->path = PATH_LAVC;
            sys->name = kCodecs[which].lavc;
            sys->frame = av_frame_alloc();
            sys->download = av_frame_alloc();
            sys->pkt = av_packet_alloc();
            opened = sys->frame && sys->download && sys->pkt && lavc_open(dec) == VLC_SUCCESS;
            if (!opened) {
                avcodec_free_context(&sys->ctx);
                av_frame_free(&sys->frame);
                av_frame_free(&sys->download);
                av_packet_free(&sys->pkt);
            }
        }
    }
    if (!opened) {
        free(sys);
        dec->p_sys = NULL;
        return VLC_EGENERIC;
    }
    /* Uncached capture memory is latency-bound per core: every core copies. A
     * cached (direct) frame needs one or two. */
    sys->copier = copier_new(sys->path == PATH_DIRECT ? 1 : COPY_WORKERS_MAX);
    if (sys->copier == NULL) {
        release(sys);
        dec->p_sys = NULL;
        return VLC_ENOMEM;
    }

    video_format_Copy(&dec->fmt_out.video, &dec->fmt_in.video);
    dec->fmt_out.i_cat = VIDEO_ES;
    dec->fmt_out.i_codec = VLC_CODEC_I420;
    dec->fmt_out.video.i_chroma = VLC_CODEC_I420;
    if (dec->fmt_out.video.i_sar_num == 0 || dec->fmt_out.video.i_sar_den == 0)
        dec->fmt_out.video.i_sar_num = dec->fmt_out.video.i_sar_den = 1;
    /* The decoder hands a frame back only several packets after its own: more
     * pictures in flight between it and the display. */
    dec->i_extra_picture_buffers = 8;
    dec->pf_decode = Decode;
    dec->pf_flush = Flush;
    msg_Info(dec, "ScreenKit decoder: %s on %s (%s, %s, %s, up to %ux%u)", sk_v4l2_codec_name(sys->sk_codec),
             sys->hw.device, sys->hw.driver, sys->hw.stateless ? "stateless" : "stateful", sys->name,
             sys->hw.max_width, sys->hw.max_height);
    return VLC_SUCCESS;
}

static void CloseDecoder(vlc_object_t *obj)
{
    decoder_t *dec = (decoder_t *)obj;
    decoder_sys_t *sys = dec->p_sys;
    if (sys == NULL)
        return;
    msg_Dbg(dec, "ScreenKit decoder: %s closed after %" PRIu64 " frames", sys->name, sys->frames);
    release(sys);
}

/* ---- the "ts" demuxer: the image's avformat, by another name ---------------------------- */

/* Which avformat module each of our demuxers borrowed, for its close. The demux's
 * p_sys is avformat's own, so the link lives here. */
static vlc_mutex_t ts_lock = VLC_STATIC_MUTEX;
static struct {
    demux_t *demux;
    module_t *module;
} ts_links[64];

static int OpenTs(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    /* Only when named: adaptive streaming asks for "ts" by name. */
    if (!demux->obj.force)
        return VLC_EGENERIC;
    var_Create(demux, "avformat-format", VLC_VAR_STRING);
    var_SetString(demux, "avformat-format", "mpegts");
    module_t *module = module_need(demux, "demux", "avformat", true);
    var_Destroy(demux, "avformat-format");
    if (module == NULL) {
        msg_Err(demux, "ScreenKit ts: the image's avformat demuxer would not take this MPEG-TS stream");
        return VLC_EGENERIC;
    }
    vlc_mutex_lock(&ts_lock);
    size_t i = 0;
    for (; i < sizeof(ts_links) / sizeof(ts_links[0]); ++i) {
        if (ts_links[i].demux == NULL) {
            ts_links[i].demux = demux;
            ts_links[i].module = module;
            break;
        }
    }
    vlc_mutex_unlock(&ts_lock);
    if (i == sizeof(ts_links) / sizeof(ts_links[0])) {
        module_unneed(demux, module);
        return VLC_EGENERIC;
    }
    msg_Dbg(demux, "ScreenKit ts: MPEG-TS through avformat");
    return VLC_SUCCESS;
}

static void CloseTs(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    module_t *module = NULL;
    vlc_mutex_lock(&ts_lock);
    for (size_t i = 0; i < sizeof(ts_links) / sizeof(ts_links[0]); ++i) {
        if (ts_links[i].demux == demux) {
            module = ts_links[i].module;
            ts_links[i].demux = NULL;
            ts_links[i].module = NULL;
            break;
        }
    }
    vlc_mutex_unlock(&ts_lock);
    if (module != NULL)
        module_unneed(demux, module);
}

/* ---- what a manifest offers ------------------------------------------------------------- */

/* The lines the player parses (MediaPlayerLinux.cpp, `ManifestLog`). The URL is
 * last, so it may hold anything but a newline. */
#define MANIFEST_MAX (1 << 20)

/* An attribute of an HLS tag's attribute list: the value of `key`, quotes
 * stripped, into `out`. */
static bool hls_attribute(const char *list, const char *key, char *out, size_t size)
{
    const size_t keylen = strlen(key);
    const char *p = list;
    while (*p != '\0' && *p != '\n' && *p != '\r') {
        while (*p == ' ' || *p == ',')
            ++p;
        const char *name = p;
        while (*p != '\0' && *p != '=' && *p != ',' && *p != '\n' && *p != '\r')
            ++p;
        if (*p != '=')
            break;
        const bool match = (size_t)(p - name) == keylen && strncmp(name, key, keylen) == 0;
        ++p;
        const char *value = p;
        size_t len;
        if (*p == '"') {
            value = ++p;
            while (*p != '\0' && *p != '"' && *p != '\n')
                ++p;
            len = (size_t)(p - value);
            if (*p == '"')
                ++p;
        } else {
            while (*p != '\0' && *p != ',' && *p != '\n' && *p != '\r')
                ++p;
            len = (size_t)(p - value);
        }
        if (match) {
            if (len >= size)
                len = size - 1;
            memcpy(out, value, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

/* The next line of `text` after `line`, or NULL. */
static const char *next_line(const char *line)
{
    const char *end = strchr(line, '\n');
    return end != NULL ? end + 1 : NULL;
}

static bool starts(const char *line, const char *prefix)
{
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

/* A media playlist: live or not, its length (VOD) or window (live), protection. */
static void hls_media(const char *text, bool *live, double *seconds, bool *protected_)
{
    bool ended = false;
    double total = 0;
    for (const char *line = text; line != NULL; line = next_line(line)) {
        if (starts(line, "#EXT-X-ENDLIST") || starts(line, "#EXT-X-PLAYLIST-TYPE:VOD"))
            ended = true;
        else if (starts(line, "#EXTINF:"))
            total += strtod(line + 8, NULL);
        else if (starts(line, "#EXT-X-KEY:")) {
            char method[32];
            if (hls_attribute(line + 11, "METHOD", method, sizeof(method)) && strncmp(method, "SAMPLE-AES", 10) == 0)
                *protected_ = true;
        }
    }
    *live = !ended;
    *seconds = total;
}

/* Up to MANIFEST_MAX bytes of `url`, read through VLC, NUL-terminated. */
static char *read_url(vlc_object_t *obj, const char *url)
{
    stream_t *s = vlc_stream_NewURL(obj, url);
    if (s == NULL)
        return NULL;
    char *text = malloc(MANIFEST_MAX + 1);
    size_t got = 0;
    if (text != NULL) {
        for (;;) {
            const ssize_t n = vlc_stream_Read(s, text + got, MANIFEST_MAX - got);
            if (n <= 0)
                break;
            got += (size_t)n;
            if (got == MANIFEST_MAX)
                break;
        }
        text[got] = '\0';
    }
    vlc_stream_Delete(s);
    return text;
}

static _Thread_local bool manifest_nested;

static void hls_manifest(stream_t *s, const char *text)
{
    bool master = false, protected_ = false, live = false;
    double seconds = 0;
    char first[4096] = "";
    for (const char *line = text; line != NULL; line = next_line(line)) {
        if (starts(line, "#EXT-X-STREAM-INF:")) {
            master = true;
            const char *attrs = line + 18;
            char value[256];
            long long bandwidth = 0;
            int width = 0, height = 0;
            double fps = 0;
            char codecs[128] = "";
            if (hls_attribute(attrs, "BANDWIDTH", value, sizeof(value)))
                bandwidth = strtoll(value, NULL, 10);
            if (hls_attribute(attrs, "RESOLUTION", value, sizeof(value)))
                sscanf(value, "%dx%d", &width, &height);
            if (hls_attribute(attrs, "FRAME-RATE", value, sizeof(value)))
                fps = strtod(value, NULL);
            hls_attribute(attrs, "CODECS", codecs, sizeof(codecs));
            /* The URI is the next line that is neither blank nor a tag. */
            const char *uri = next_line(line);
            while (uri != NULL && (*uri == '#' || *uri == '\n' || *uri == '\r'))
                uri = next_line(uri);
            char ref[4096] = "";
            if (uri != NULL) {
                size_t len = strcspn(uri, "\r\n");
                if (len >= sizeof(ref))
                    len = sizeof(ref) - 1;
                memcpy(ref, uri, len);
                ref[len] = '\0';
            }
            char *resolved = ref[0] != '\0' ? vlc_uri_resolve(s->psz_url, ref) : NULL;
            if (first[0] == '\0' && resolved != NULL)
                snprintf(first, sizeof(first), "%s", resolved);
            msg_Info(s, "ScreenKit variant: kind=video bandwidth=%lld width=%d height=%d fps=%.3f codecs=%s "
                        "manifest=%s uri=%s", bandwidth, width, height, fps, codecs[0] ? codecs : "-",
                     s->psz_url, resolved != NULL ? resolved : "-");
            free(resolved);
        } else if (starts(line, "#EXT-X-SESSION-KEY:")) {
            char method[32];
            if (hls_attribute(line + 19, "METHOD", method, sizeof(method)) && strncmp(method, "SAMPLE-AES", 10) == 0)
                protected_ = true;
        }
    }
    if (!master) {
        hls_media(text, &live, &seconds, &protected_);
    } else if (first[0] != '\0') {
        /* A master playlist says nothing of liveness: its first variant does. */
        manifest_nested = true;
        char *media = read_url(VLC_OBJECT(s), first);
        manifest_nested = false;
        if (media != NULL) {
            hls_media(media, &live, &seconds, &protected_);
            free(media);
        }
    }
    msg_Info(s, "ScreenKit manifest: type=hls live=%d seconds=%.3f protected=%d url=%s", live, seconds, protected_,
             s->psz_url);
}

/* An ISO 8601 duration ("PT1H2M3.5S", "P0Y0M0DT0H0M10.000S") in seconds. */
static double iso_duration(const char *text)
{
    double total = 0;
    bool time = false;
    const char *p = text;
    if (*p == 'P')
        ++p;
    while (*p != '\0' && *p != '"') {
        if (*p == 'T') {
            time = true;
            ++p;
            continue;
        }
        char *end;
        const double value = strtod(p, &end);
        if (end == p)
            break;
        switch (*end) {
            case 'Y': total += value * 365 * 86400; break;
            case 'M': total += time ? value * 60 : value * 30 * 86400; break;
            case 'W': total += value * 7 * 86400; break;
            case 'D': total += value * 86400; break;
            case 'H': total += value * 3600; break;
            case 'S': total += value; break;
            default: return total;
        }
        p = end + 1;
    }
    return total;
}

/* The value of XML attribute `name` in the tag starting at `tag`, into `out`. */
static bool xml_attribute(const char *tag, const char *name, char *out, size_t size)
{
    const char *end = strchr(tag, '>');
    const size_t namelen = strlen(name);
    for (const char *p = tag; p != NULL && (end == NULL || p < end); ++p) {
        p = strstr(p, name);
        if (p == NULL || (end != NULL && p >= end))
            return false;
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') && p[namelen] == '=' &&
            (p[namelen + 1] == '"' || p[namelen + 1] == '\'')) {
            const char quote = p[namelen + 1];
            const char *value = p + namelen + 2;
            const char *close = strchr(value, quote);
            if (close == NULL)
                return false;
            size_t len = (size_t)(close - value);
            if (len >= size)
                len = size - 1;
            memcpy(out, value, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

static void dash_manifest(stream_t *s, const char *text)
{
    const char *mpd = strstr(text, "<MPD");
    if (mpd == NULL)
        return;
    char value[256];
    const bool live = xml_attribute(mpd, "type", value, sizeof(value)) && strcmp(value, "dynamic") == 0;
    double seconds = 0;
    if (xml_attribute(mpd, live ? "timeShiftBufferDepth" : "mediaPresentationDuration", value, sizeof(value)))
        seconds = iso_duration(value);
    const bool protected_ = strstr(text, "<ContentProtection") != NULL;

    for (const char *set = strstr(text, "<AdaptationSet"); set != NULL; set = strstr(set + 1, "<AdaptationSet")) {
        const char *set_end = strstr(set, "</AdaptationSet>");
        char set_type[64] = "", set_codecs[128] = "", set_fps[32] = "";
        xml_attribute(set, "contentType", set_type, sizeof(set_type));
        if (set_type[0] == '\0' && xml_attribute(set, "mimeType", value, sizeof(value)))
            snprintf(set_type, sizeof(set_type), "%.*s", (int)strcspn(value, "/"), value);
        xml_attribute(set, "codecs", set_codecs, sizeof(set_codecs));
        xml_attribute(set, "frameRate", set_fps, sizeof(set_fps));
        for (const char *rep = strstr(set, "<Representation"); rep != NULL && (set_end == NULL || rep < set_end);
             rep = strstr(rep + 1, "<Representation")) {
            if (strncmp(rep, "<RepresentationIndex", 20) == 0)
                continue;
            char type[64], codecs[128], fps[32];
            snprintf(type, sizeof(type), "%s", set_type);
            snprintf(codecs, sizeof(codecs), "%s", set_codecs);
            snprintf(fps, sizeof(fps), "%s", set_fps);
            if (xml_attribute(rep, "mimeType", value, sizeof(value)))
                snprintf(type, sizeof(type), "%.*s", (int)strcspn(value, "/"), value);
            xml_attribute(rep, "codecs", codecs, sizeof(codecs));
            xml_attribute(rep, "frameRate", fps, sizeof(fps));
            long long bandwidth = 0;
            int width = 0, height = 0;
            if (xml_attribute(rep, "bandwidth", value, sizeof(value)))
                bandwidth = strtoll(value, NULL, 10);
            if (xml_attribute(rep, "width", value, sizeof(value)))
                width = atoi(value);
            if (xml_attribute(rep, "height", value, sizeof(value)))
                height = atoi(value);
            double rate = 0;
            if (fps[0] != '\0') {
                int num = 0, den = 1;
                if (sscanf(fps, "%d/%d", &num, &den) == 2 && den > 0)
                    rate = (double)num / den;
                else
                    rate = strtod(fps, NULL);
            }
            const char *kind = strcmp(type, "video") == 0 ? "video" : strcmp(type, "audio") == 0 ? "audio" : NULL;
            if (kind == NULL)
                continue;
            msg_Info(s, "ScreenKit variant: kind=%s bandwidth=%lld width=%d height=%d fps=%.3f codecs=%s manifest=%s uri=-",
                     kind, bandwidth, width, height, rate, codecs[0] ? codecs : "-", s->psz_url);
        }
    }
    msg_Info(s, "ScreenKit manifest: type=dash live=%d seconds=%.3f protected=%d url=%s", live, seconds, protected_,
             s->psz_url);
}

static int OpenManifest(vlc_object_t *obj)
{
    stream_t *s = (stream_t *)obj;
    if (manifest_nested || s->p_source == NULL || s->psz_url == NULL)
        return VLC_EGENERIC;
    const uint8_t *peek;
    ssize_t n = vlc_stream_Peek(s->p_source, &peek, 512);
    if (n < 7)
        return VLC_EGENERIC;
    const size_t bom = (n >= 3 && memcmp(peek, "\xEF\xBB\xBF", 3) == 0) ? 3 : 0;
    bool hls = (size_t)n >= bom + 7 && memcmp(peek + bom, "#EXTM3U", 7) == 0;
    bool dash = false;
    if (!hls) {
        /* An MPD is XML: a declaration, perhaps comments, then <MPD. */
        const char *start = (const char *)peek + bom;
        while (start < (const char *)peek + n && (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t'))
            ++start;
        if (start >= (const char *)peek + n || *start != '<')
            return VLC_EGENERIC;
        for (ssize_t i = 0; i + 4 <= n && !dash; ++i)
            dash = memcmp(peek + i, "<MPD", 4) == 0;
        if (!dash)
            return VLC_EGENERIC;
    }
    n = vlc_stream_Peek(s->p_source, &peek, MANIFEST_MAX);
    if (n <= 0)
        return VLC_EGENERIC;
    char *text = malloc((size_t)n + 1);
    if (text == NULL)
        return VLC_EGENERIC;
    memcpy(text, peek, (size_t)n);
    text[n] = '\0';
    if (hls)
        hls_manifest(s, text);
    else
        dash_manifest(s, text);
    free(text);
    return VLC_EGENERIC;
}
