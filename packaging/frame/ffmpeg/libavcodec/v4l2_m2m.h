/*
 * V4L2 mem2mem helper functions
 *
 * Copyright (C) 2017 Alexis Ballier <aballier@gentoo.org>
 * Copyright (C) 2017 Jorge Ramirez <jorge.ramirez-ortiz@linaro.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Modified by John Cox for rpi-ffmpeg (https://github.com/jc-kynesim/rpi-ffmpeg,
 * commit d0fe46c27ff1, July 2024) and, in 2026, by the VideoFrameVR project
 * for the Steam Frame. packaging/frame/ffmpeg/README.md in VideoFrameVR's
 * source lists the sources, the changes and their licence.
 */

#ifndef AVCODEC_V4L2_M2M_H
#define AVCODEC_V4L2_M2M_H

#include <semaphore.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/videodev2.h>

#include "libavcodec/avcodec.h"
#include "libavutil/pixfmt.h"
#include "v4l2_context.h"

#define container_of(ptr, type, member) ({ \
        const __typeof__(((type *)0)->member ) *__mptr = (ptr); \
        (type *)((char *)__mptr - offsetof(type,member) );})

#define V4L_M2M_DEFAULT_OPTS \
    { "num_output_buffers", "Number of buffers in the output context",\
        OFFSET(num_output_buffers), AV_OPT_TYPE_INT, { .i64 = 16 }, 2, INT_MAX, FLAGS }

#define FF_V4L2_M2M_TRACK_SIZE 128
typedef struct V4L2m2mTrackEl {
    int     discard;   // If we see this buffer its been flushed, so discard
    int     pending;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int64_t track_pts;
} V4L2m2mTrackEl;

typedef struct pts_stats_s
{
    void * logctx;
    const char * name;  // For debug
    unsigned int last_count;
    unsigned int last_interval;
    int64_t last_pts;
    int64_t guess;
} pts_stats_t;

typedef struct xlat_track_s {
    unsigned int track_no;
    int64_t last_pts;    // Last valid PTS decoded
    V4L2m2mTrackEl track_els[FF_V4L2_M2M_TRACK_SIZE];
} xlat_track_t;

struct dmabufs_ctl;

enum IrisStartup {
    IRIS_START_OFF,
    IRIS_START_FIRST_PACKET,
    IRIS_START_SOURCE_CHANGE,
    IRIS_START_FIRST_FRAME,
    IRIS_START_READY,
};

typedef struct V4L2m2mContext {
    char devname[PATH_MAX];
    int fd;

    /* the codec context queues */
    V4L2Context capture;
    V4L2Context output;

    /* dynamic stream reconfig */
    AVCodecContext *avctx;
    sem_t refsync;
    atomic_uint refcount;

    /* null frame/packet received */
    int draining;
    int running;
    AVPacket buf_pkt;

    /* Reference to a frame. Only used during encoding */
    AVFrame *frame;

    /* Reference to self; only valid while codec is active. */
    AVBufferRef *self_ref;

    /* reference back to V4L2m2mPriv */
    void *priv;

    AVBufferRef *device_ref;

    /* generate DRM frames */
    int output_drm;

    /* input frames are drmprime */
    int input_drm;

    /* Frame tracking */
    xlat_track_t xlat;

    pts_stats_t pts_stat;

    /* req pkt */
    int req_pkt;
    int reorder_size;
    enum IrisStartup iris_startup;
    int64_t iris_startup_deadline;
    int64_t iris_seek_deadline;
    // No usable SPS in the codec header: the first packet's decides.
    int iris_sps_in_band;
    // H.264's DPB size for the staged pool; its reorder_size stays the
    // reorder depth the scheduler uses.
    int iris_h264_dpb;
    // Why the codec header's SPS can't take the staged start.
    const char *iris_staged_refusal;
    // The SPS's coded size, which Iris reports at the source change. The
    // context's can be the conformance-cropped size the container gives.
    int iris_coded_width;
    int iris_coded_height;
    // The first SPS whose Q08C frames would be laid out other than an
    // importer expects: its coded and visible sizes (0 if none).
    int q08c_crop_coded_width;
    int q08c_crop_coded_height;
    int q08c_crop_width;
    int q08c_crop_height;
    int iris_staged_eligible;
    struct v4l2_format iris_capture_format;

    /* Set from another thread (av_v4l2_m2m_set_interrupted()): a decoder
     * waiting for its caller to return a capture buffer gives up instead.
     * interrupt_lock covers the queues' lock and cond while they exist. */
    atomic_int interrupted;
    pthread_mutex_t interrupt_lock;

    /* Ext data sent */
    int extdata_sent;
    /* Ext data sent in packet - overrides ctx */
    void * extdata_data;
    size_t extdata_size;

#define FF_V4L2_QUIRK_REINIT_ALWAYS             1
#define FF_V4L2_QUIRK_ENUM_FRAMESIZES_BROKEN    2
#define FF_V4L2_QUIRK_IRIS_DRAIN_SEEK          4
    /* Quirks */
    unsigned int quirks;

    struct dmabufs_ctl * db_ctl;
} V4L2m2mContext;

// The DPB the staged start's pool covers. This fork keeps HEVC's in
// reorder_size.
static inline int ff_v4l2_iris_staged_dpb(const V4L2m2mContext *s)
{
    return s->avctx && s->avctx->codec_id == AV_CODEC_ID_H264 ? s->iris_h264_dpb : s->reorder_size;
}

// Full range changes no sample layout: yuvj420p decodes as yuv420p.
static inline enum AVPixelFormat ff_v4l2_iris_layout(const enum AVPixelFormat pix_fmt)
{
    return pix_fmt == AV_PIX_FMT_YUVJ420P ? AV_PIX_FMT_YUV420P : pix_fmt;
}

static inline int64_t ff_v4l2_iris_deadline(const V4L2m2mContext *s)
{
    if (s->iris_seek_deadline)
        return s->iris_seek_deadline;
    return s->iris_startup == IRIS_START_SOURCE_CHANGE ||
           s->iris_startup == IRIS_START_FIRST_FRAME ? s->iris_startup_deadline : 0;
}

typedef struct V4L2m2mPriv {
    AVClass *class;

    V4L2m2mContext *context;
    AVBufferRef    *context_ref;

    int num_output_buffers;
    int num_capture_buffers;
    int capture_spare;
    int drain_seek_status;
    int capture_ubwc;
    int capture_10bit;
    enum AVPixelFormat source_pix_fmt;
    const char * dmabuf_alloc;
    enum AVPixelFormat pix_fmt;
} V4L2m2mPriv;

/**
 * Allocate a new context and references for a V4L2 M2M instance.
 *
 * @param[in] ctx The V4L2m2mPriv instantiated by the encoder/decoder.
 * @param[out] ctx The V4L2m2mContext.
 *
 * @returns 0 in success, a negative error code otherwise.
 */
int ff_v4l2_m2m_create_context(V4L2m2mPriv *priv, V4L2m2mContext **s);


/**
 * Probes the video nodes looking for the required codec capabilities.
 *
 * @param[in] ctx The V4L2m2mPriv instantiated by the encoder/decoder.
 *
 * @returns 0 if a driver is found, a negative number otherwise.
 */
int ff_v4l2_m2m_codec_init(V4L2m2mPriv *priv);

/**
 * Releases all the codec resources if all AVBufferRefs have been returned to the
 * ctx. Otherwise keep the driver open.
 *
 * @param[in] The V4L2m2mPriv instantiated by the encoder/decoder.
 *
 * @returns 0
 *
 */
int ff_v4l2_m2m_codec_end(V4L2m2mPriv *priv);

/**
 * Reinitializes the V4L2m2mContext when the driver cannot continue processing
 * with the capture parameters.
 *
 * @param[in] ctx The V4L2m2mContext instantiated by the encoder/decoder.
 *
 * @returns 0 in case of success, negative number otherwise
 */
int ff_v4l2_m2m_codec_reinit(V4L2m2mContext *ctx);

/**
 * Reinitializes the V4L2m2mContext when the driver cannot continue processing
 * with the  any of the current V4L2Contexts (ie, changes in output and capture).
 *
 * @param[in] ctx The V4L2m2mContext instantiated by the encoder/decoder.
 *
 * @returns 0 in case of success, negative number otherwise
 */
int ff_v4l2_m2m_codec_full_reinit(V4L2m2mContext *ctx);

/**
 * Exported for callers that decode on one thread and return frames on
 * another: while interrupted, a V4L2 decoder that has no capture buffer
 * left (all of them held as decoded frames) returns AVERROR(EAGAIN) from
 * receive_frame at once instead of waiting for one to be returned. The
 * caller sets it from any thread before it needs the decoding thread back
 * (e.g. to flush) and clears it once it has it. Must not race with
 * avcodec_open2() or avcodec_free_context() of avctx.
 *
 * @returns 0, or AVERROR(ENOSYS) if avctx is not an open V4L2 decoder
 */
int av_v4l2_m2m_set_interrupted(AVCodecContext *avctx, int interrupted);


static inline unsigned int ff_v4l2_get_format_width(const struct v4l2_format * const fmt)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.width : fmt->fmt.pix.width;
}

static inline unsigned int ff_v4l2_get_format_height(const struct v4l2_format * const fmt)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.height : fmt->fmt.pix.height;
}

static inline uint32_t ff_v4l2_get_format_pixelformat(const struct v4l2_format * const fmt)
{
    return V4L2_TYPE_IS_MULTIPLANAR(fmt->type) ? fmt->fmt.pix_mp.pixelformat : fmt->fmt.pix.pixelformat;
}

static inline int ff_v4l2_ctx_eos(const V4L2Context * const ctx)
{
    return ctx->flag_last;
}


#endif /* AVCODEC_V4L2_M2M_H */
