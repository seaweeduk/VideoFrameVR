/*
 * V4L2 mem2mem decoders
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

#include "config_components.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "libavutil/avassert.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bytestream.h"
#include "codec_internal.h"
#include "libavcodec/decode.h"

#include "libavcodec/hwaccels.h"
#include "libavcodec/internal.h"
#include "libavcodec/hwconfig.h"

#include "v4l2_context.h"
#include "v4l2_m2m.h"
#include "v4l2_fmt.h"
#include "v4l2_req_dmabufs.h"

#if CONFIG_H264_DECODER
#include "h264_parse.h"
#endif
#if CONFIG_HEVC_DECODER
#include "h2645_parse.h"
#include "hevc/parse.h"
#endif

// Pick 64 for max last count - that is >1sec at 60fps
#define STATS_LAST_COUNT_MAX 64
#define STATS_INTERVAL_MAX (1 << 30)

#ifndef FF_API_BUFFER_SIZE_T
#define FF_API_BUFFER_SIZE_T 1
#endif

#define DUMP_FAILED_EXTRADATA 0

#if DUMP_FAILED_EXTRADATA
static inline char hex1(unsigned int x)
{
    x &= 0xf;
    return x <= 9 ? '0' + x : 'a' + x - 10;
}

static inline char * hex2(char * s, unsigned int x)
{
    *s++ = hex1(x >> 4);
    *s++ = hex1(x);
    return s;
}

static inline char * hex4(char * s, unsigned int x)
{
    s = hex2(s, x >> 8);
    s = hex2(s, x);
    return s;
}

static inline char * dash2(char * s)
{
    *s++ = '-';
    *s++ = '-';
    return s;
}

static void
data16(char * s, const unsigned int offset, const uint8_t * m, const size_t len)
{
    size_t i;
    s = hex4(s, offset);
    m += offset;
    for (i = 0; i != 8; ++i) {
        *s++ = ' ';
        s = len > i + offset ? hex2(s, *m++) : dash2(s);
    }
    *s++ = ' ';
    *s++ = ':';
    for (; i != 16; ++i) {
        *s++ = ' ';
        s = len > i + offset ? hex2(s, *m++) : dash2(s);
    }
    *s++ = 0;
}

static void
log_dump(void * logctx, int lvl, const void * const data, const size_t len)
{
    size_t i;
    for (i = 0; i < len; i += 16) {
        char buf[80];
        data16(buf, i, data, len);
        av_log(logctx, lvl, "%s\n", buf);
    }
}
#endif

static unsigned int pts_stats_interval(const pts_stats_t * const stats)
{
    return stats->last_interval;
}

static int64_t pts_stats_guess(const pts_stats_t * const stats, const int fail_bad_guess)
{
    if (stats->last_count <= 1)
        return stats->last_pts;
    if (stats->last_pts == AV_NOPTS_VALUE ||
            fail_bad_guess && (stats->last_interval == 0 ||
                               stats->last_count >= STATS_LAST_COUNT_MAX))
        return AV_NOPTS_VALUE;
    return stats->last_pts + (int64_t)(stats->last_count - 1) * (int64_t)stats->last_interval;
}

static void pts_stats_add(pts_stats_t * const stats, int64_t pts)
{
    if (pts == AV_NOPTS_VALUE || pts == stats->last_pts) {
        if (stats->last_count < STATS_LAST_COUNT_MAX)
            ++stats->last_count;
        return;
    }

    if (stats->last_pts != AV_NOPTS_VALUE) {
        const int64_t interval = pts - stats->last_pts;

        if (interval < 0 || interval >= STATS_INTERVAL_MAX ||
            stats->last_count >= STATS_LAST_COUNT_MAX) {
            if (stats->last_interval != 0)
                av_log(stats->logctx, AV_LOG_DEBUG, "%s: %s: Bad interval: %" PRId64 "/%d\n",
                       __func__, stats->name, interval, stats->last_count);
            stats->last_interval = 0;
        }
        else {
            const int64_t frame_time = interval / (int64_t)stats->last_count;

            if (frame_time != stats->last_interval)
                av_log(stats->logctx, AV_LOG_DEBUG, "%s: %s: New interval: %u->%" PRId64 "/%d=%" PRId64 "\n",
                       __func__, stats->name, stats->last_interval, interval, stats->last_count, frame_time);
            stats->last_interval = frame_time;
        }
    }

    stats->last_pts = pts;
    stats->last_count = 1;
}

static void pts_stats_init(pts_stats_t * const stats, void * logctx, const char * name)
{
    *stats = (pts_stats_t){
        .logctx = logctx,
        .name = name,
        .last_count = 1,
        .last_interval = 0,
        .last_pts = AV_NOPTS_VALUE
    };
}

// If abdata == NULL then this just counts space required
// Unpacks avcC if detected
static int
h264_xd_copy(const uint8_t * const extradata, const int extrasize, uint8_t * abdata)
{
    const uint8_t * const xdend = extradata + extrasize;
    const uint8_t * p = extradata;
    uint8_t * d = abdata;
    unsigned int n;
    unsigned int len;
    const unsigned int hdrlen = 4;
    unsigned int need_pps = 1;

    if (extrasize < 8)
        return AVERROR(EINVAL);

    if (p[0] == 0 && p[1] == 0) {
        // Assume a couple of leading zeros are good enough to indicate NAL
        if (abdata)
            memcpy(d, p, extrasize);
        return extrasize;
    }

    // avcC starts with a 1
    if (p[0] != 1)
        return AVERROR(EINVAL);

    p += 5;
    n = *p++ & 0x1f;

doxps:
    while (n--) {
        if (xdend - p < 2)
            return AVERROR(EINVAL);
        len = (p[0] << 8) | p[1];
        p += 2;
        if (xdend - p < (ptrdiff_t)len)
            return AVERROR(EINVAL);
        if (abdata) {
            d[0] = 0;
            d[1] = 0;
            d[2] = 0;
            d[3] = 1;
            memcpy(d + 4, p, len);
        }
        d += len + hdrlen;
        p += len;
    }
    if (need_pps) {
        need_pps = 0;
        if (p >= xdend)
            return AVERROR(EINVAL);
        n = *p++;
        goto doxps;
    }

    return d - abdata;
}

static int
copy_extradata(AVCodecContext * const avctx,
               const void * const src_data, const int src_len,
               void ** const pdst_data, size_t * const pdst_len)
{
    int len;

    *pdst_len = 0;
    av_freep(pdst_data);

    if (avctx->codec_id == AV_CODEC_ID_H264)
        len = h264_xd_copy(src_data, src_len, NULL);
    else
        len = src_len < 0 ? AVERROR(EINVAL) : src_len;

    // Zero length is OK but we want to stop - -ve is error val
    if (len <= 0)
        return len;

    if ((*pdst_data = av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE)) == NULL)
        return AVERROR(ENOMEM);

    if (avctx->codec_id == AV_CODEC_ID_H264)
        h264_xd_copy(src_data, src_len, *pdst_data);
    else
        memcpy(*pdst_data, src_data, len);
    *pdst_len = len;

    return 0;
}



static int check_output_streamon(AVCodecContext *const avctx, V4L2m2mContext *const s)
{
    int ret;
    struct v4l2_decoder_cmd cmd = {
        .cmd = V4L2_DEC_CMD_START,
        .flags = 0,
    };

    if (s->output.streamon)
        return 0;

    ret = ff_v4l2_context_set_status(&s->output, VIDIOC_STREAMON);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "VIDIOC_STREAMON on output context: %s\n", av_err2str(ret));
        return ret;
    }

    if (s->iris_startup)
        return 0;

    // STREAMON should do implicit START so this just for those that don't.
    // It is optional so don't worry if it fails
    if (ioctl(s->fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
        ret = AVERROR(errno);
        // Iris refuses it (EBUSY) unless a drain or a resolution change waits.
        av_log(avctx, ret == AVERROR(EBUSY) ? AV_LOG_DEBUG : AV_LOG_WARNING,
               "VIDIOC_DECODER_CMD start error: %s\n", av_err2str(ret));
    }
    else {
        av_log(avctx, AV_LOG_TRACE, "VIDIOC_DECODER_CMD start OK\n");
    }
    return 0;
}

static int v4l2_try_start(AVCodecContext *avctx)
{
    V4L2m2mContext * const s = ((V4L2m2mPriv*)avctx->priv_data)->context;
    int ret;

    /* 1. start the output process */
    if ((ret = check_output_streamon(avctx, s)) != 0)
        return ret;
    return 0;
}

static int v4l2_prepare_decoder(V4L2m2mContext *s)
{
    struct v4l2_event_subscription sub;
    V4L2Context *output = &s->output;
    int ret;

    /**
     * requirements
     */
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    ret = ioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if ( ret < 0) {
        if (output->height == 0 || output->width == 0) {
            av_log(s->avctx, AV_LOG_ERROR,
                "the v4l2 driver does not support VIDIOC_SUBSCRIBE_EVENT\n"
                "you must provide codec_height and codec_width on input\n");
            return ret;
        }
    }

    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_EOS;
    ret = ioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if (ret < 0)
        av_log(s->avctx, AV_LOG_WARNING,
               "the v4l2 driver does not support end of stream VIDIOC_SUBSCRIBE_EVENT\n");

    return 0;
}

static void
set_best_effort_pts(AVCodecContext *const avctx,
             pts_stats_t * const ps,
             AVFrame *const frame)
{
    pts_stats_add(ps, frame->pts);

    frame->best_effort_timestamp = pts_stats_guess(ps, 1);
    // If we can't guess from just PTS - try DTS
    if (frame->best_effort_timestamp == AV_NOPTS_VALUE)
        frame->best_effort_timestamp = frame->pkt_dts;

    // We can't emulate what s/w does in a useful manner and using the
    // "correct" answer seems to just confuse things.
    frame->pkt_dts               = frame->pts;
    av_log(avctx, AV_LOG_TRACE, "Out PTS=%" PRId64 "/%"PRId64", DTS=%" PRId64 "\n",
           frame->pts, frame->best_effort_timestamp, frame->pkt_dts);
}

static void
xlat_flush(xlat_track_t * const x)
{
    unsigned int i;
    // Do not reset track_no - this ensures that any frames left in the decoder
    // that turn up later get discarded.

    x->last_pts = AV_NOPTS_VALUE;
    for (i = 0; i != FF_V4L2_M2M_TRACK_SIZE; ++i) {
        x->track_els[i].pending = 0;
        x->track_els[i].discard = 1;
    }
}

static void
xlat_init(xlat_track_t * const x)
{
    memset(x, 0, sizeof(*x));
    xlat_flush(x);
}

static int
xlat_pending(const V4L2m2mContext * const s)
{
    const xlat_track_t *const x = &s->xlat;
    unsigned int n = x->track_no % FF_V4L2_M2M_TRACK_SIZE;
    int i;
    const int64_t now = pts_stats_guess(&s->pts_stat, 0);
    int64_t first_dts = AV_NOPTS_VALUE;
    int no_dts_count = 0;
    unsigned int interval = pts_stats_interval(&s->pts_stat);

    for (i = 0; i < FF_V4L2_M2M_TRACK_SIZE; ++i, n = (n - 1) & (FF_V4L2_M2M_TRACK_SIZE - 1)) {
        const V4L2m2mTrackEl * const t = x->track_els + n;

        if (first_dts == AV_NOPTS_VALUE)
            if (t->dts == AV_NOPTS_VALUE)
                ++no_dts_count;
            else
                first_dts = t->dts;

        // Discard only set on never-set or flushed entries
        // So if we get here we've never successfully decoded a frame so allow
        // more frames into the buffer before stalling
        if (t->discard)
            return i - 16;

        // If we've got this frame out then everything before this point
        // must have entered the decoder
        if (!t->pending)
            break;

        // If we've never seen a pts all we can do is count frames
        if (now == AV_NOPTS_VALUE)
            continue;

        if (t->dts != AV_NOPTS_VALUE && now >= t->dts)
            break;
    }

    if (first_dts != AV_NOPTS_VALUE && now != AV_NOPTS_VALUE && interval != 0 && s->reorder_size != 0) {
        const int iframes = (first_dts - now) / (int)interval;
        const int t = iframes - s->reorder_size + no_dts_count;

//        av_log(s->avctx, AV_LOG_DEBUG, "Last:%"PRId64", Now:%"PRId64", First:%"PRId64", delta=%"PRId64", frames=%d, nodts=%d\n",
//               x->last_dts, now, first_dts, first_dts - now, iframes, no_dts_count);

        if (iframes > 0 && iframes < 64 && t < i) {
            return t;
        }
    }

    return i;
}

static inline int stream_started(const V4L2m2mContext * const s) {
    return s->output.streamon;
}

#define NQ_OK        0
#define NQ_Q_FULL    1
#define NQ_SRC_EMPTY 2
#define NQ_NONE      3
#define NQ_DRAINING  4
#define NQ_DEAD      5

#define TRY_DQ(nq_status) ((nq_status) >= NQ_OK && (nq_status) <= NQ_DRAINING)
#define RETRY_NQ(nq_status) ((nq_status) == NQ_Q_FULL || (nq_status) == NQ_NONE)

// do_not_get      If true then no new packet will be got but status will
//                  be set appropriately

// AVERROR_EOF     Flushing an already flushed stream
// -ve             Error (all errors except EOF are unexpected)
// NQ_OK (0)       OK
// NQ_Q_FULL       Dst full (retry if we think V4L2 Q has space now)
// NQ_SRC_EMPTY    Src empty (do not retry)
// NQ_NONE         Enqueue not attempted
// NQ_DRAINING     At EOS, dQ dest until EOS there too
// NQ_DEAD         Not running (do not retry, do not attempt capture dQ)

#if CONFIG_HEVC_DECODER
// Whether every SPS fits the staged start's pool, raising *reorder_size to
// their deepest reference count, and their one coded size. Main 10 allows
// 8-bit pictures, and the level doesn't size the pool. The context has the
// coded size, or the size an SPS's right and bottom conformance window crop
// leaves: decoded frames can't carry a left or top crop. Otherwise *refusal
// says why not.
static int iris_staged_sps_fit(const AVCodecContext * const avctx, const HEVCParamSets * const ps,
                               const enum AVPixelFormat pix_fmt, int * const reorder_size,
                               int * const coded_width, int * const coded_height,
                               const char ** const refusal)
{
    int found = 0;

    *coded_width = *coded_height = 0;
    *refusal = NULL;
    for (unsigned int i = 0; i != HEVC_MAX_SPS_COUNT; ++i) {
        const HEVCSPS *p = ps->sps_list[i];
        const HEVCWindow *crop;
        if (!p)
            continue;
        crop = &p->output_window;
        found = 1;
        if (!*coded_width) {
            *coded_width = p->width;
            *coded_height = p->height;
        }
        if (p->width != *coded_width || p->height != *coded_height) {
            *refusal = "SPS sizes differ";
            continue;
        }
        if (crop->left_offset || crop->top_offset) {
            *refusal = "SPS crops left or top";
            continue;
        }
        if ((p->width != avctx->coded_width || p->height != avctx->coded_height) &&
            (p->width - (int)crop->right_offset != avctx->width ||
             p->height - (int)crop->bottom_offset != avctx->height)) {
            *refusal = "SPS size or crop doesn't match the stream";
            continue;
        }
        if (((p->width + 15) >> 4) * ((p->height + 15) >> 4) > (8192 / 16) * (4320 / 16)) {
            *refusal = "SPS size above the session limit";
            continue;
        }
        if (ff_v4l2_iris_layout(p->pix_fmt) != ff_v4l2_iris_layout(pix_fmt) ||
            p->chroma_format_idc != 1 ||
            (p->bit_depth != 8 && p->bit_depth != 10) ||
            p->bit_depth_chroma != p->bit_depth ||
            (p->ptl.general_ptl.profile_idc != 2 &&
             (p->ptl.general_ptl.profile_idc != 1 || p->bit_depth != 8)) ||
            p->max_sub_layers < 1 || p->max_sub_layers > 7) {
            *refusal = "SPS format, profile or sub-layers";
            continue;
        }
        for (unsigned int j = 0; j < p->max_sub_layers; ++j) {
            const int dpb = p->temporal_layer[j].max_dec_pic_buffering;
            if (dpb < 1 || dpb > 6)
                *refusal = "SPS DPB outside 1-6";
            *reorder_size = FFMAX(*reorder_size, dpb);
        }
    }
    if (!found)
        *refusal = "no SPS";
    return !*refusal;
}

// For a codec header without a usable SPS (hev1, or an hvcC missing its
// VPS): the first packet's parameter sets. That packet is a keyframe, after
// a start or a seek alike.
static int iris_staged_packet_fits(AVCodecContext * const avctx, V4L2m2mContext * const s,
                                   const AVPacket * const pkt, const char ** const refusal)
{
    HEVCParamSets ps = {0};
    H2645Packet h = {0};
    int ret = ff_h2645_packet_split(&h, pkt->data, pkt->size, avctx, 0, AV_CODEC_ID_HEVC,
                                    H2645_FLAG_SMALL_PADDING);
    int fit = 0;

    *refusal = "unreadable parameter sets";

    // VPSs first: an SPS needs its VPS, which may follow it. Any set that
    // can't be read fails the packet, so no SPS goes unchecked.
    for (int sps = 0; ret >= 0 && sps < 2; ++sps) {
        for (int i = 0; ret >= 0 && i < h.nb_nals; ++i) {
            H2645NAL *nal = &h.nals[i];
            if (nal->nuh_layer_id > 0 || nal->type != (sps ? HEVC_NAL_SPS : HEVC_NAL_VPS))
                continue;
            ret = sps ? ff_hevc_decode_nal_sps(&nal->gb, avctx, &ps, nal->nuh_layer_id, 0) :
                        ff_hevc_decode_nal_vps(&nal->gb, avctx, &ps);
        }
    }
    if (ret >= 0) {
        s->reorder_size = 0;
        fit = iris_staged_sps_fit(avctx, &ps, ((V4L2m2mPriv *)avctx->priv_data)->source_pix_fmt,
                                  &s->reorder_size, &s->iris_coded_width, &s->iris_coded_height,
                                  refusal);
    }
    ff_h2645_packet_uninit(&h);
    ff_hevc_ps_uninit(&ps);
    return fit;
}
#endif

static int try_enqueue_src(AVCodecContext * const avctx, V4L2m2mContext * const s, const int do_not_get)
{
    int ret;

    if (ff_v4l2_iris_deadline(s)) {
        if (av_gettime_relative() >= ff_v4l2_iris_deadline(s))
            return AVERROR(ETIMEDOUT);
        if (s->iris_startup == IRIS_START_SOURCE_CHANGE)
            return NQ_NONE;
    }

    // If we don't already have a coded packet - get a new one
    // We will already have a coded pkt if the output Q was full last time we
    // tried to Q it
    if (!s->buf_pkt.size && !do_not_get) {
        unsigned int i;

        for (i = 0; i < 256; ++i) {
            uint8_t * side_data;
            size_t side_size;

            ret = ff_decode_get_packet(avctx, &s->buf_pkt);
            if (ret != 0)
                break;

            // New extradata is the only side-data we undertand
            side_data = av_packet_get_side_data(&s->buf_pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
            if (side_data) {
                av_log(avctx, AV_LOG_DEBUG, "New extradata\n");
                if ((ret = copy_extradata(avctx, side_data, (int)side_size, &s->extdata_data, &s->extdata_size)) < 0)
                    av_log(avctx, AV_LOG_WARNING, "Failed to copy new extra data: %s\n", av_err2str(ret));
                s->extdata_sent = 0;
            }

            if (s->buf_pkt.size != 0)
                break;

            if (s->buf_pkt.side_data_elems == 0) {
                av_log(avctx, AV_LOG_WARNING, "Empty pkt from ff_decode_get_packet - treating as EOF\n");
                ret = AVERROR_EOF;
                break;
            }

            // Retry a side-data only pkt
        }
        // If i >= 256 something has gone wrong
        if (i >= 256) {
            av_log(avctx, AV_LOG_ERROR, "Too many side-data only packets\n");
            return AVERROR(EIO);
        }

        if (ret == AVERROR(EAGAIN)) {
            if (!stream_started(s)) {
                av_log(avctx, AV_LOG_TRACE, "%s: receive_frame before 1st coded packet\n", __func__);
                return NQ_DEAD;
            }
            return NQ_SRC_EMPTY;
        }

        if (ret == AVERROR_EOF) {
            // EOF - enter drain mode
            av_log(avctx, AV_LOG_TRACE, "--- EOS req: ret=%d, size=%d, started=%d, drain=%d\n",
                   ret, s->buf_pkt.size, stream_started(s), s->draining);
            if (!stream_started(s)) {
                av_log(avctx, AV_LOG_DEBUG, "EOS on flushed stream\n");
                s->draining = 1;
                s->capture.done = 1;
                return AVERROR_EOF;
            }

            if (!s->draining) {
                // Calling enqueue with an empty pkt starts drain
                av_assert0(s->buf_pkt.size == 0);
                ret = ff_v4l2_context_enqueue_packet(&s->output, &s->buf_pkt, NULL, 0);
                if (ret) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to start drain: ret=%d\n", ret);
                    return ret;
                }
            }
            return NQ_DRAINING;
        }

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get coded packet: err=%d\n", ret);
            return ret;
        }
    }

    if (s->draining) {
        if (s->buf_pkt.size) {
            av_log(avctx, AV_LOG_WARNING, "Unexpected input whilst draining\n");
            av_packet_unref(&s->buf_pkt);
        }
        return NQ_DRAINING;
    }

    if (!s->buf_pkt.size)
        return NQ_NONE;

    if ((ret = check_output_streamon(avctx, s)) != 0)
        return ret;

    if (s->iris_startup == IRIS_START_FIRST_PACKET) {
#if CONFIG_HEVC_DECODER
        if (s->iris_sps_in_band) {
            const char *refusal;
            if (!iris_staged_packet_fits(avctx, s, &s->buf_pkt, &refusal)) {
                av_log(avctx, AV_LOG_ERROR, "Iris staged startup: the stream's SPS doesn't fit (%s)\n",
                       refusal);
                return AVERROR(ENOSYS);
            }
            s->iris_sps_in_band = 0;
        }
#endif
        if (!s->output_drm || ff_v4l2_iris_staged_dpb(s) < 1 || ff_v4l2_iris_staged_dpb(s) > 6) {
            av_log(avctx, AV_LOG_ERROR, "Iris staged startup: needs DRM PRIME output and a DPB of 1-6\n");
            return AVERROR(ENOSYS);
        }
        // Both queues stream before the stream's headers arrive.
        if (!s->capture.streamon) {
            ret = ff_v4l2_context_set_status(&s->capture, VIDIOC_STREAMON);
            if (ret < 0)
                return ret;
        }
    }
    if (s->extdata_sent)
        ret = ff_v4l2_context_enqueue_packet(&s->output, &s->buf_pkt, NULL, 0);
    else
        ret = ff_v4l2_context_enqueue_packet(&s->output, &s->buf_pkt, s->extdata_data, s->extdata_size);

    if (ret == AVERROR(EAGAIN)) {
        // Out of input buffers - keep packet
        ret = NQ_Q_FULL;
    }
    else {
        // In all other cases we are done with this packet
        av_packet_unref(&s->buf_pkt);
        s->extdata_sent = 1;

        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Packet enqueue failure: err=%d\n", ret);
            return ret;
        }
        if (s->iris_startup == IRIS_START_FIRST_PACKET) {
            s->iris_startup = IRIS_START_SOURCE_CHANGE;
            s->iris_startup_deadline = av_gettime_relative() + 2000000;
        }
    }

    // Start if we haven't
    {
        const int ret2 = v4l2_try_start(avctx);
        if (ret2) {
            av_log(avctx, AV_LOG_DEBUG, "Start failure: err=%d\n", ret2);
            ret = (ret2 == AVERROR(ENOMEM)) ? ret2 : NQ_DEAD;
        }
    }

    return ret;
}

static int qbuf_wait(AVCodecContext * const avctx, V4L2Context * const ctx)
{
    int rv = 0;
    const V4L2m2mContext *s = ((V4L2m2mPriv *)avctx->priv_data)->context;

    ff_mutex_lock(&ctx->lock);

    while (atomic_load(&ctx->q_count) == 0 && ctx->streamon) {
        int err;
        const int64_t deadline = ff_v4l2_iris_deadline(s);
        if (deadline) {
            const int64_t remaining = deadline - av_gettime_relative();
            struct timespec until;
            if (remaining <= 0) {
                rv = AVERROR(ETIMEDOUT);
                break;
            }
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += remaining / 1000000;
            until.tv_nsec += (remaining % 1000000) * 1000;
            until.tv_sec += until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            err = pthread_cond_timedwait(&ctx->cond, &ctx->lock, &until);
        } else {
            // Only the caller returning a frame frees a buffer here, so a
            // caller that needs this thread first (to flush) interrupts it.
            // Checked under the lock its broadcast takes: never missed.
            if (atomic_load(&s->interrupted)) {
                rv = AVERROR(EINTR);
                break;
            }
            err = pthread_cond_wait(&ctx->cond, &ctx->lock);
        }
        if (err != 0) {
            rv = AVERROR(err);
            av_log(avctx, AV_LOG_ERROR, "Cond wait failure: %s\n", av_err2str(rv));
            break;
        }
    }

    ff_mutex_unlock(&ctx->lock);
    return rv;
}

int av_v4l2_m2m_set_interrupted(AVCodecContext *avctx, int interrupted)
{
    V4L2m2mContext *s;

    if (!avctx || !avctx->codec || !av_codec_is_decoder(avctx->codec) ||
        !avctx->codec->wrapper_name || strcmp(avctx->codec->wrapper_name, "v4l2m2m") ||
        !avcodec_is_open(avctx) || !avctx->priv_data)
        return AVERROR(ENOSYS);
    s = ((V4L2m2mPriv *)avctx->priv_data)->context;
    if (!s)
        return AVERROR(ENOSYS);

    atomic_store(&s->interrupted, !!interrupted);
    if (interrupted) {
        pthread_mutex_lock(&s->interrupt_lock);
        if (s->capture.sync_ready) {
            ff_mutex_lock(&s->capture.lock);
            pthread_cond_broadcast(&s->capture.cond);
            ff_mutex_unlock(&s->capture.lock);
        }
        pthread_mutex_unlock(&s->interrupt_lock);
    }
    return 0;
}

static int v4l2_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    V4L2m2mContext *const s = ((V4L2m2mPriv*)avctx->priv_data)->context;
    int src_rv = -1;
    int dst_rv = 1;  // Non-zero (done), non-negative (error) number
    unsigned int i = 0;

    do {
        const int pending = xlat_pending(s);
        const int prefer_dq = (pending > 4);
        const int last_src_rv = src_rv;

        av_log(avctx, AV_LOG_TRACE, "Pending=%d, src_rv=%d, req_pkt=%d\n", pending, src_rv, s->req_pkt);

        // Enqueue another pkt for decode if
        // (a) We don't have a lot of stuff in the buffer already OR
        // (b) ... we (think we) do but we've failed to get a frame already OR
        // (c) We've dequeued a lot of frames without asking for input
        src_rv = try_enqueue_src(avctx, s, !(!prefer_dq || i != 0 || s->req_pkt > 2));

        // If we got a frame last time or we've already tried to get a frame and
        // we have nothing to enqueue then return now. rv will be AVERROR(EAGAIN)
        // indicating that we want more input.
        // This should mean that once decode starts we enter a stable state where
        // we alternately ask for input and produce output
        if ((i != 0 || s->req_pkt) && src_rv == NQ_SRC_EMPTY)
            break;

        if (src_rv == NQ_Q_FULL && last_src_rv == NQ_Q_FULL) {
            av_log(avctx, AV_LOG_WARNING, "Poll thinks src Q has space; none found\n");
            break;
        }

        // Try to get a new frame if
        // (a) we haven't already got one AND
        // (b) enqueue returned a status indicating that decode should be attempted
        if (dst_rv != 0 && TRY_DQ(src_rv)) {
            // Pick a timeout depending on state
            // The pending count isn't completely reliable so it is good enough
            // hint that we want a frame but not good enough to require it in
            // all cases; however if it has got > 31 that exceeds its margin of
            // error so require a frame to prevent ridiculous levels of latency
            const int t = ff_v4l2_iris_deadline(s) ? 20 :
                src_rv == NQ_Q_FULL ? -1 :
                src_rv == NQ_DRAINING ? 300 :
                prefer_dq ? (s->running && pending > 31 ? 100 : 5) : 0;

            // Dequeue frame will unref any previous contents of frame
            // if it returns success so we don't need an explicit unref
            // when discarding
            // This returns AVERROR(EAGAIN) on timeout or if
            // there is room in the input Q and timeout == -1
            dst_rv = ff_v4l2_context_dequeue_frame(&s->capture, frame, t);

            // Failure due to no buffer in Q?
            if (dst_rv == AVERROR(ENOSPC)) {
                // Wait & retry
                if ((dst_rv = qbuf_wait(avctx, &s->capture)) == 0) {
                    dst_rv = ff_v4l2_context_dequeue_frame(&s->capture, frame, t);
                } else if (dst_rv == AVERROR(EINTR)) {
                    // No frame now; the caller calls again once it is done.
                    av_frame_unref(frame);
                    s->req_pkt = 0;
                    return AVERROR(EAGAIN);
                }
            }

            if (dst_rv == 0) {
                if (s->iris_startup == IRIS_START_FIRST_FRAME)
                    s->iris_startup = IRIS_START_READY;
                set_best_effort_pts(avctx, &s->pts_stat, frame);
                if (!s->running) {
                    s->running = 1;
                    av_log(avctx, AV_LOG_VERBOSE, "Decode running\n");
                }
            }

            if (dst_rv == AVERROR(EAGAIN) && src_rv == NQ_DRAINING &&
                !(s->iris_startup && s->iris_startup != IRIS_START_READY)) {
                av_log(avctx, AV_LOG_WARNING, "Timeout in drain - assume EOF");
                dst_rv = AVERROR_EOF;
                s->capture.done = 1;
            }
            else if (dst_rv == AVERROR_EOF && (s->draining || s->capture.done))
                av_log(avctx, AV_LOG_DEBUG, "Dequeue EOF: draining=%d, cap.done=%d\n",
                       s->draining, s->capture.done);
            else if (dst_rv && dst_rv != AVERROR(EAGAIN))
                av_log(avctx, AV_LOG_ERROR, "Packet dequeue failure: draining=%d, cap.done=%d, err=%d\n",
                       s->draining, s->capture.done, dst_rv);
            // Preserve a failed capture allocation instead of consuming more
            // packets and replacing ENOMEM with the stopped queue's EOF.
            if (dst_rv == AVERROR(ENOMEM) ||
                (s->iris_startup && dst_rv < 0 && dst_rv != AVERROR(EAGAIN) &&
                 dst_rv != AVERROR(ENOSPC) && dst_rv != AVERROR_INVALIDDATA)) {
                av_frame_unref(frame);
                if (dst_rv == AVERROR_EOF && !s->draining)
                    return AVERROR(EIO);
                return dst_rv;
            }
        }

        ++i;
        if (i >= 256) {
            av_log(avctx, AV_LOG_ERROR, "Unexpectedly large retry count: %d\n", i);
            src_rv = AVERROR(EIO);
        }

        // Continue trying to enqueue packets if either
        // (a) we succeeded last time OR
        // (b) we didn't ret a frame and we can retry the input
    } while (src_rv == NQ_OK || (dst_rv == AVERROR(EAGAIN) && RETRY_NQ(src_rv)));

    // Ensure that the frame contains nothing if we aren't returning a frame
    // (might happen when discarding)
    if (dst_rv)
        av_frame_unref(frame);

    // If we got a frame this time ask for a pkt next time
    s->req_pkt = (dst_rv == 0) ? s->req_pkt + 1 : 0;

#if 0
    if (dst_rv == 0)
    {
        static int z = 0;
        if (++z > 50) {
            av_log(avctx, AV_LOG_ERROR, "Streamoff and die?\n");
            ff_v4l2_context_set_status(&s->capture, VIDIOC_STREAMOFF);
            return -1;
        }
    }
#endif

    return dst_rv == 0 ? 0 :
        src_rv < 0 ? src_rv :
        dst_rv < 0 ? dst_rv :
            AVERROR(EAGAIN);
}

#if 0
#include <time.h>
static int64_t us_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int v4l2_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int ret;
    const int64_t now = us_time();
    int64_t done;
    av_log(avctx, AV_LOG_TRACE, "<<< %s\n", __func__);
    ret = v4l2_receive_frame2(avctx, frame);
    done = us_time();
    av_log(avctx, AV_LOG_TRACE, ">>> %s: rx time=%" PRId64 ", rv=%d\n", __func__, done - now, ret);
    return ret;
}
#endif

static uint32_t
avprofile_to_v4l2(const enum AVCodecID codec_id, const int avprofile)
{
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            switch (avprofile) {
                case AV_PROFILE_H264_BASELINE:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
                case AV_PROFILE_H264_CONSTRAINED_BASELINE:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
                case AV_PROFILE_H264_MAIN:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
                case AV_PROFILE_H264_EXTENDED:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED;
                case AV_PROFILE_H264_HIGH:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
                case AV_PROFILE_H264_HIGH_10:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10;
                case AV_PROFILE_H264_HIGH_10_INTRA:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10_INTRA;
                case AV_PROFILE_H264_MULTIVIEW_HIGH:
                case AV_PROFILE_H264_HIGH_422:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422;
                case AV_PROFILE_H264_HIGH_422_INTRA:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422_INTRA;
                case AV_PROFILE_H264_STEREO_HIGH:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_STEREO_HIGH;
                case AV_PROFILE_H264_HIGH_444_PREDICTIVE:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
                case AV_PROFILE_H264_HIGH_444_INTRA:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_INTRA;
                case AV_PROFILE_H264_CAVLC_444:
                    return V4L2_MPEG_VIDEO_H264_PROFILE_CAVLC_444_INTRA;
                case AV_PROFILE_H264_HIGH_444:
                default:
                    break;
//                    V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_BASELINE		= 12,
//                    V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH		= 13,
//                    V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH_INTRA	= 14,
//                    V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH		= 16,
//                    V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH		= 17,
            }
            break;
        case AV_CODEC_ID_MPEG2VIDEO:
        case AV_CODEC_ID_MPEG4:
        case AV_CODEC_ID_VC1:
        case AV_CODEC_ID_VP8:
        case AV_CODEC_ID_VP9:
        case AV_CODEC_ID_AV1:
            // Most profiles are a simple number that matches the V4L2 enum
            return avprofile;
        default:
            break;
    }
    return ~(uint32_t)0;
}

// This check mirrors Chrome's profile check by testing to see if the profile
// exists as a possible value for the V4L2 profile control
static int
check_profile(AVCodecContext *const avctx, V4L2m2mContext *const s)
{
    struct v4l2_queryctrl query_ctrl;
    struct v4l2_querymenu query_menu;
    uint32_t profile_id;

    // An unset profile is almost certainly zero or -99 - do not reject
    if (avctx->profile <= 0) {
        av_log(avctx, AV_LOG_VERBOSE, "Profile %d <= 0 - check skipped\n", avctx->profile);
        return 0;
    }

    memset(&query_ctrl, 0, sizeof(query_ctrl));
    switch (avctx->codec_id) {
        case AV_CODEC_ID_MPEG2VIDEO:
            profile_id = V4L2_CID_MPEG_VIDEO_MPEG2_PROFILE;
            break;
        case AV_CODEC_ID_MPEG4:
            profile_id = V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE;
            break;
        case AV_CODEC_ID_H264:
            profile_id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
            break;
        case AV_CODEC_ID_VP8:
            profile_id = V4L2_CID_MPEG_VIDEO_VP8_PROFILE;
            break;
        case AV_CODEC_ID_VP9:
            profile_id = V4L2_CID_MPEG_VIDEO_VP9_PROFILE;
            break;
#ifdef V4L2_CID_MPEG_VIDEO_AV1_PROFILE
        case AV_CODEC_ID_AV1:
            profile_id = V4L2_CID_MPEG_VIDEO_AV1_PROFILE;
            break;
#endif
        default:
            av_log(avctx, AV_LOG_VERBOSE, "Can't map profile for codec id %d; profile check skipped\n", avctx->codec_id);
            return 0;
    }

    query_ctrl = (struct v4l2_queryctrl){.id = profile_id};
    if (ioctl(s->fd, VIDIOC_QUERYCTRL, &query_ctrl) != 0) {
        av_log(avctx, AV_LOG_VERBOSE, "Query profile ctrl (%#x) not supported: assume OK\n", query_ctrl.id);
    }
    else {
        av_log(avctx, AV_LOG_DEBUG, "%s: Control supported: %#x\n", __func__, query_ctrl.id);

        query_menu = (struct v4l2_querymenu){
            .id = query_ctrl.id,
            .index = avprofile_to_v4l2(avctx->codec_id, avctx->profile),
        };

        if (query_menu.index > query_ctrl.maximum ||
            query_menu.index < query_ctrl.minimum ||
            ioctl(s->fd, VIDIOC_QUERYMENU, &query_menu) != 0) {
            return AVERROR(ENOENT);
        }
    }

    return 0;
};

static int
check_size(AVCodecContext * const avctx, V4L2m2mContext * const s, const uint32_t fcc)
{
    unsigned int i;
    const uint32_t w = avctx->coded_width;
    const uint32_t h = avctx->coded_height;

    if (w == 0 || h == 0 || fcc == 0) {
        av_log(avctx, AV_LOG_TRACE, "%s: Size %dx%d or fcc %s empty\n", __func__, w, h, av_fourcc2str(fcc));
        return 0;
    }
    if ((s->quirks & FF_V4L2_QUIRK_ENUM_FRAMESIZES_BROKEN) != 0) {
        av_log(avctx, AV_LOG_TRACE, "%s: Skipped (quirk): Size %dx%d, fcc %s\n", __func__, w, h, av_fourcc2str(fcc));
        return 0;
    }

    for (i = 0;; ++i) {
        struct v4l2_frmsizeenum fs = {
            .index = i,
            .pixel_format = fcc,
        };

        while (ioctl(s->fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) {
            const int err = AVERROR(errno);
            if (err == AVERROR(EINTR))
                continue;
            if (i == 0 && err == AVERROR(ENOTTY)) {
                av_log(avctx, AV_LOG_DEBUG, "Framesize enum not supported\n");
                return 0;
            }
            if (err != AVERROR(EINVAL)) {
                av_log(avctx, AV_LOG_ERROR, "Failed to enum framesizes: %s", av_err2str(err));
                return err;
            }
            av_log(avctx, AV_LOG_WARNING, "Failed to find Size=%dx%d, fmt=%s in %u frame size enums\n",
                   w, h, av_fourcc2str(fcc), i);
            return err;
        }

        switch (fs.type) {
            case V4L2_FRMSIZE_TYPE_DISCRETE:
                av_log(avctx, AV_LOG_TRACE, "%s[%d]: Discrete: %dx%d\n", __func__, i,
                       fs.discrete.width,fs.discrete.height);
                if (w == fs.discrete.width && h == fs.discrete.height)
                    return 0;
                break;
            case V4L2_FRMSIZE_TYPE_STEPWISE:
                av_log(avctx, AV_LOG_TRACE, "%s[%d]: Stepwise: Min: %dx%d Max: %dx%d, Step: %dx%d\n", __func__, i,
                       fs.stepwise.min_width, fs.stepwise.min_height,
                       fs.stepwise.max_width, fs.stepwise.max_height,
                       fs.stepwise.step_width,fs.stepwise.step_height);
                if (w >= fs.stepwise.min_width && w <= fs.stepwise.max_width &&
                    h >= fs.stepwise.min_height && h <= fs.stepwise.max_height &&
                    (w - fs.stepwise.min_width) % fs.stepwise.step_width == 0 &&
                    (h - fs.stepwise.min_height) % fs.stepwise.step_height == 0)
                    return 0;
                break;
            case V4L2_FRMSIZE_TYPE_CONTINUOUS:
                av_log(avctx, AV_LOG_TRACE, "%s[%d]: Continuous: Min: %dx%d Max: %dx%d, Step: %dx%d\n", __func__, i,
                       fs.stepwise.min_width, fs.stepwise.min_height,
                       fs.stepwise.max_width, fs.stepwise.max_height,
                       fs.stepwise.step_width,fs.stepwise.step_height);
                if (w >= fs.stepwise.min_width && w <= fs.stepwise.max_width &&
                    h >= fs.stepwise.min_height && h <= fs.stepwise.max_height)
                    return 0;
                break;
            default:
                av_log(avctx, AV_LOG_ERROR, "Unexpected framesize enum: %d", fs.type);
                return AVERROR(EINVAL);
        }
    }
}

static int
get_quirks(AVCodecContext * const avctx, V4L2m2mContext * const s)
{
    struct v4l2_capability cap;

    memset(&cap, 0, sizeof(cap));
    while (ioctl(s->fd, VIDIOC_QUERYCAP, &cap) != 0) {
        int err = errno;
        if (err == EINTR)
            continue;
        av_log(avctx, AV_LOG_ERROR, "V4L2: Failed to get capabilities: %s\n", strerror(err));
        return AVERROR(err);
    }

    // Could be made table driven if we have a few more but right now there
    // seems no point

    // Meson (amlogic) always gives a resolution changed event after output
    // streamon and userspace must (re)allocate capture buffers and streamon
    // capture to clear the event even if the capture buffers were the right
    // size in the first place.
    if (strcmp(cap.driver, "meson-vdec") == 0)
        s->quirks |= FF_V4L2_QUIRK_REINIT_ALWAYS | FF_V4L2_QUIRK_ENUM_FRAMESIZES_BROKEN;

    if (strcmp(cap.driver, "iris_driver") == 0 && avctx->codec_id == AV_CODEC_ID_HEVC)
        s->quirks |= FF_V4L2_QUIRK_IRIS_DRAIN_SEEK;

    av_log(avctx, AV_LOG_DEBUG, "Driver '%s': Quirks=%#x\n", cap.driver, s->quirks);
    return 0;
}

// This heuristic is for H264 but use for everything
static uint32_t max_coded_size(const AVCodecContext * const avctx)
{
    uint32_t wxh = avctx->coded_width * avctx->coded_height;
    uint32_t size;

    size = wxh * 3 / 2;
    // H.264 Annex A table A-1 gives minCR which is either 2 or 4
    // unfortunately that doesn't yield an actually useful limit
    // and it should be noted that frame 0 is special cased to allow
    // a bigger number which really isn't helpful for us. So just pick
    // frame_size / 2
    size /= 2;
    // Add 64k to allow for any overheads and/or encoder hopefulness
    // with small WxH
    return size + (1 << 16);
}

#if CONFIG_H264_DECODER
// Whether every SPS fits the staged start's pool, and the deepest DPB they
// need: the stream's own, else its level's at this size (FFmpeg's level
// table stops at 5.2), never fewer than its reference frames. Otherwise
// *refusal says why not.
static int iris_staged_h264_fit(const AVCodecContext * const avctx, const H264ParamSets * const ps,
                                int * const dpb_max, const char ** const refusal)
{
    *dpb_max = 0;
    *refusal = "no SPS";
    for (unsigned int i = 0; i != MAX_SPS_COUNT; ++i) {
        const SPS *p = ps->sps_list[i];
        int dpb;
        if (!p)
            continue;
        if (p->mb_width * 16 != avctx->coded_width || p->mb_height * 16 != avctx->coded_height ||
            p->crop || !p->frame_mbs_only_flag) {
            *refusal = "SPS cropped, interlaced or not the stream's size";
            return 0;
        }
        if (p->chroma_format_idc != 1 || p->bit_depth_luma != 8 || p->bit_depth_chroma != 8 ||
            (p->profile_idc != 77 && p->profile_idc != 100 &&
             (p->profile_idc != 66 || !(p->constraint_set_flags & 2)))) {
            *refusal = "SPS format or profile";
            return 0;
        }
        dpb = p->bitstream_restriction_flag ? p->max_dec_frame_buffering :
              p->level_idc >= 60 && p->level_idc <= 62 ? 696320 / (p->mb_width * p->mb_height) : 16;
        dpb = FFMAX(dpb, p->ref_frame_count);
        if (dpb < 1 || dpb > 6) {
            *refusal = "SPS DPB outside 1-6";
            return 0;
        }
        *dpb_max = FFMAX(*dpb_max, dpb);
        *refusal = NULL;
    }
    return !*refusal;
}
#endif

// Whether Q08C frames laid out for this coded size, as the capture format
// has them, differ from frames of the visible size, from which importers
// create their images and the driver derives the planes' metadata sizes.
// Compared: the pitch and both metadata sizes, which only change at
// multiples of 128 rows and columns, so a crop within a block never does.
// The chroma offset and the buffer size go to the importers with the frame.
static int q08c_crop_differs(const int coded_width, const int coded_height,
                             const int width, const int height)
{
    const V4L2UbwcSizes coded = ff_v4l2_ubwc_sizes(coded_width, coded_height, 0);
    const V4L2UbwcSizes visible = ff_v4l2_ubwc_sizes(width, height, 0);
    return coded.pitch != visible.pitch || coded.y_meta != visible.y_meta ||
           coded.uv_meta != visible.uv_meta;
}

static void q08c_crop_note(V4L2m2mContext * const s, const int coded_width, const int coded_height,
                           const int width, const int height)
{
    if (s->q08c_crop_coded_width || !q08c_crop_differs(coded_width, coded_height, width, height))
        return;
    s->q08c_crop_coded_width = coded_width;
    s->q08c_crop_coded_height = coded_height;
    s->q08c_crop_width = width;
    s->q08c_crop_height = height;
}

#if CONFIG_HEVC_DECODER
// FFmpeg's ff_hevc_decode_extradata() (hevc/parse.c), reading the base layer
// only, as it did up to 7.0. Later versions also read a multilayer stream's
// other layers (MV-HEVC's second view, an alpha layer): their SPSs would
// count in the size, crop and buffer checks here, and one can replace a base
// layer's SPS of the same id. Iris decodes the base layer.
static int hevc_base_nal_units(const uint8_t *buf, int buf_size, HEVCParamSets *ps,
                               HEVCSEI *sei, int is_nalff, int nal_length_size,
                               int err_recognition, void *logctx)
{
    int ret;
    int flags = (H2645_FLAG_IS_NALFF * !!is_nalff) | H2645_FLAG_SMALL_PADDING;
    H2645Packet pkt = { 0 };

    ret = ff_h2645_packet_split(&pkt, buf, buf_size, logctx,
                                nal_length_size, AV_CODEC_ID_HEVC, flags);

    for (int i = 0; ret >= 0 && i < pkt.nb_nals; i++) {
        H2645NAL *nal = &pkt.nals[i];
        if (nal->nuh_layer_id > 0)
            continue;
        switch (nal->type) {
        case HEVC_NAL_VPS:
            ret = ff_hevc_decode_nal_vps(&nal->gb, logctx, ps);
            break;
        case HEVC_NAL_SPS:
            ret = ff_hevc_decode_nal_sps(&nal->gb, logctx, ps, 0, 0);
            break;
        case HEVC_NAL_PPS:
            ret = ff_hevc_decode_nal_pps(&nal->gb, logctx, ps);
            break;
        case HEVC_NAL_SEI_PREFIX:
        case HEVC_NAL_SEI_SUFFIX:
            ret = ff_hevc_decode_nal_sei(&nal->gb, logctx, sei, ps, nal->type);
            break;
        default:
            break;
        }
    }

    ff_h2645_packet_uninit(&pkt);
    return (err_recognition & AV_EF_EXPLODE) ? ret : 0;
}

static int hevc_base_extradata(const uint8_t *data, int size, HEVCParamSets *ps,
                               HEVCSEI *sei, int err_recognition, void *logctx)
{
    GetByteContext gb;
    int num_arrays;

    // Not hvcC: Annex B.
    if (!(size >= 23 && (data[0] == 1 || (data[0] == 0 && (data[1] || data[2] > 1)))))
        return hevc_base_nal_units(data, size, ps, sei, 0, 0, err_recognition, logctx);

    bytestream2_init(&gb, data, size);
    bytestream2_skip(&gb, 22);
    num_arrays = bytestream2_get_byte(&gb);

    for (int i = 0; i < num_arrays; i++) {
        int cnt;

        bytestream2_skip(&gb, 1);
        cnt = bytestream2_get_be16(&gb);

        for (int j = 0; j < cnt; j++) {
            // hvcC's NAL units have a two-byte length each.
            int nalsize = bytestream2_peek_be16(&gb) + 2;
            int ret;

            if (bytestream2_get_bytes_left(&gb) < nalsize)
                return AVERROR_INVALIDDATA;
            ret = hevc_base_nal_units(gb.buffer, nalsize, ps, sei, 1, 2,
                                      err_recognition, logctx);
            if (ret < 0)
                return ret;
            bytestream2_skip(&gb, nalsize);
        }
    }
    return 0;
}
#endif

static void
parse_extradata(AVCodecContext * const avctx, V4L2m2mContext * const s)
{
    s->reorder_size = 0;
    s->iris_h264_dpb = 0;
    s->iris_staged_eligible = 0;
    s->iris_staged_refusal = "no readable SPS";
    s->iris_coded_width = s->iris_coded_height = 0;
    s->q08c_crop_coded_width = s->q08c_crop_coded_height = 0;
    s->q08c_crop_width = s->q08c_crop_height = 0;
    s->iris_sps_in_band = avctx->codec_id == AV_CODEC_ID_HEVC;

    if (!avctx->extradata || !avctx->extradata_size)
        return;

    switch (avctx->codec_id) {
#if CONFIG_H264_DECODER
        case AV_CODEC_ID_H264:
        {
            H264ParamSets ps;
            int is_avc = 0;
            int nal_length_size = 0;
            int ret;

            memset(&ps, 0, sizeof(ps));

            ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                           &ps, &is_avc, &nal_length_size,
                                           avctx->err_recognition, avctx);
            if (ret >= 0) {
                const SPS * sps = NULL;
                unsigned int i;
                for (i = 0; i != MAX_SPS_COUNT; ++i) {
                    if (ps.sps_list[i]) {
                        sps = ps.sps_list[i];
                        break;
                    }
                }
                // The crops are in luma samples, mb_height counts frame rows.
                for (i = 0; i != MAX_SPS_COUNT; ++i) {
                    const SPS *p = ps.sps_list[i];
                    if (p)
                        q08c_crop_note(s, p->mb_width * 16, p->mb_height * 16,
                                       p->mb_width * 16 - (int)(p->crop_left + p->crop_right),
                                       p->mb_height * 16 - (int)(p->crop_top + p->crop_bottom));
                }
                if (sps) {
                    avctx->profile = ff_h264_get_profile(sps);
                    avctx->level = sps->level_idc;
                    s->reorder_size = sps->num_reorder_frames;
                    s->iris_staged_eligible = iris_staged_h264_fit(avctx, &ps, &s->iris_h264_dpb,
                                                                   &s->iris_staged_refusal);
                    if (s->iris_staged_eligible) {
                        s->iris_coded_width = avctx->coded_width;
                        s->iris_coded_height = avctx->coded_height;
                    }
                }
            }
            ff_h264_ps_uninit(&ps);
            break;
        }
#endif
#if CONFIG_HEVC_DECODER
        case AV_CODEC_ID_HEVC:
        {
            HEVCParamSets ps;
            HEVCSEI sei;
            int ret;

            memset(&ps, 0, sizeof(ps));
            memset(&sei, 0, sizeof(sei));

            ret = hevc_base_extradata(avctx->extradata, avctx->extradata_size,
                                      &ps, &sei, avctx->err_recognition, avctx);
            if (ret >= 0) {
                const HEVCSPS * sps = NULL;
                unsigned int i;
                for (i = 0; i != HEVC_MAX_SPS_COUNT; ++i) {
                    if (ps.sps_list[i]) {
                        sps = ps.sps_list[i];
                        break;
                    }
                }
                for (i = 0; i != HEVC_MAX_SPS_COUNT; ++i) {
                    const HEVCSPS *p = ps.sps_list[i];
                    if (p)
                        q08c_crop_note(s, p->width, p->height,
                                       p->width - (int)(p->output_window.left_offset +
                                                        p->output_window.right_offset),
                                       p->height - (int)(p->output_window.top_offset +
                                                         p->output_window.bottom_offset));
                }
                if (sps) {
                    avctx->profile = sps->ptl.general_ptl.profile_idc;
                    ((V4L2m2mPriv *)avctx->priv_data)->source_pix_fmt = sps->pix_fmt;
                    avctx->level   = sps->ptl.general_ptl.level_idc;
                    s->reorder_size = sps->temporal_layer[sps->max_sub_layers - 1].max_dec_pic_buffering;
                    s->iris_staged_eligible = iris_staged_sps_fit(avctx, &ps, sps->pix_fmt, &s->reorder_size,
                                                                  &s->iris_coded_width,
                                                                  &s->iris_coded_height,
                                                                  &s->iris_staged_refusal);
                    s->iris_sps_in_band = 0;
                }
            }
            ff_hevc_ps_uninit(&ps);
            ff_hevc_reset_sei(&sei);
            break;
        }
#endif
        default:
            break;
    }
}

static int
choose_capture_format(AVCodecContext * const avctx, V4L2m2mContext * const s)
{
    const V4L2m2mPriv * const priv = avctx->priv_data;
    unsigned int fmts_n;
    uint32_t *fmts = ff_v4l2_context_enum_drm_formats(&s->capture, &fmts_n);
    enum AVPixelFormat *fmts2 = NULL;
    enum AVPixelFormat gf_pix_fmt;
    unsigned int i;
    unsigned int n = 0;
    unsigned int pref_n = 1;
    int rv = AVERROR(ENOENT);

    if (!fmts)
        return AVERROR(ENOENT);

    if ((fmts2 = av_malloc(sizeof(*fmts2) * (fmts_n + 3))) == NULL) {
        rv = AVERROR(ENOMEM);
        goto error;
    }

    // Filter for formats that are supported by ffmpeg and
    // can accomodate the stream size
    fmts2[n++] = AV_PIX_FMT_DRM_PRIME;
    for (i = 0; i != fmts_n; ++i) {
        const enum AVPixelFormat f = ff_v4l2_format_v4l2_to_avfmt(fmts[i], AV_CODEC_ID_RAWVIDEO);
        av_log(avctx, AV_LOG_TRACE, "VLC pix %s -> %s\n", av_fourcc2str(fmts[i]), av_get_pix_fmt_name(f));
        if (f == AV_PIX_FMT_NONE)
            continue;

        if (check_size(avctx, s, fmts[i]) != 0)
            continue;

        if (f == priv->pix_fmt)
            pref_n = n;
        fmts2[n++] = f;
    }

    if (n < 2) {
        av_log(avctx, AV_LOG_DEBUG, "%s: No usable formats found\n", __func__);
        goto error;
    }

    if (n != 2) {
        // ffmpeg.c really only expects one s/w format. It thinks that the
        // last format in the list is the s/w format of the h/w format but
        // also chooses the first non-h/w format as the preferred s/w format.
        // The only way of reconciling this is to dup our preferred format into
        // both last & first place :-(
        const enum AVPixelFormat t = fmts2[pref_n];
        fmts2[pref_n] = fmts2[1];
        fmts2[1] = t;
        fmts2[n++] = t;
    }

    fmts2[n] = AV_PIX_FMT_NONE;

    gf_pix_fmt = ff_get_format(avctx, fmts2);
    av_log(avctx, AV_LOG_DEBUG, "avctx requested=%d (%s) %dx%d; get_format requested=%d (%s)\n",
           avctx->pix_fmt, av_get_pix_fmt_name(avctx->pix_fmt),
           avctx->coded_width, avctx->coded_height,
           gf_pix_fmt, av_get_pix_fmt_name(gf_pix_fmt));

    if (gf_pix_fmt == AV_PIX_FMT_NONE)
        goto error;

    if (gf_pix_fmt == AV_PIX_FMT_DRM_PRIME || avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
        s->capture.av_pix_fmt = avctx->sw_pix_fmt;
        s->output_drm = 1;
    }
    else {
        avctx->pix_fmt = gf_pix_fmt;
        s->capture.av_pix_fmt = gf_pix_fmt;
        s->output_drm = 0;
    }

    if (priv->capture_ubwc && !s->output_drm) {
        rv = AVERROR(ENOSYS);
        goto error;
    }

    // Get format converts capture.av_pix_fmt back into a V4L2 format in the context
    if ((rv = ff_v4l2_context_get_format(&s->capture, 0)) != 0)
        goto error;
    if (s->iris_startup) {
        // OUTPUT already has buffers. Reapplying full-size CAPTURE S_FMT
        // here would change the admission rectangle after the final input
        // S_FMT. The full-size compressed format is already programmed.
        rv = ioctl(s->fd, VIDIOC_G_FMT, &s->capture.format) < 0 ? AVERROR(errno) : 0;
        if (!rv) {
            const struct v4l2_pix_format_mplane *p = &s->capture.format.fmt.pix_mp;
            const struct v4l2_pix_format_mplane *pool = &s->iris_capture_format.fmt.pix_mp;
            if (!s->output_drm || p->pixelformat != pool->pixelformat ||
                p->num_planes != 1 || pool->num_planes != 1 ||
                p->field != pool->field || p->width != pool->width || p->height != pool->height ||
                p->plane_fmt[0].bytesperline != pool->plane_fmt[0].bytesperline ||
                p->plane_fmt[0].sizeimage != pool->plane_fmt[0].sizeimage)
                rv = AVERROR(EINVAL);
        }
        goto error;
    }
    rv = ff_v4l2_context_set_format(&s->capture);

error:
    av_free(fmts2);
    av_free(fmts);
    return rv;
}

static av_cold int v4l2_decode_init(AVCodecContext *avctx)
{
    V4L2Context *capture, *output;
    V4L2m2mContext *s;
    V4L2m2mPriv *priv = avctx->priv_data;
    int ret;

    av_log(avctx, AV_LOG_TRACE, "<<< %s\n", __func__);

    ret = ff_v4l2_m2m_create_context(priv, &s);
    if (ret < 0)
        return ret;

    priv->source_pix_fmt = avctx->pix_fmt;
    parse_extradata(avctx, s);

    // Iris Main 10 must use compressed capture; linear NV12 is not a
    // bit-depth conversion and can make firmware access invalid DPB memory.
    const AVPixFmtDescriptor *source = av_pix_fmt_desc_get(priv->source_pix_fmt);
    priv->capture_10bit = source && source->comp[0].depth == 10 &&
                         avctx->codec_id == AV_CODEC_ID_HEVC;
    if ((source && (source->nb_components != 3 || source->log2_chroma_w != 1 ||
                    source->log2_chroma_h != 1 ||
                    (source->comp[0].depth != 8 && !priv->capture_10bit))) ||
        (!source && avctx->codec_id == AV_CODEC_ID_HEVC &&
         avctx->profile == AV_PROFILE_HEVC_MAIN_10)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported Iris source bit depth or chroma\n");
        return AVERROR(ENOSYS);
    }
    if (priv->capture_10bit)
        priv->capture_ubwc = 1;
    // Q08C frames are laid out for the SPS's coded size, but importers
    // only know the visible size: a crop that changes the layout gets
    // linear NV12. Sizes from parameter sets in packets are not checked.
    else if (priv->capture_ubwc && s->q08c_crop_coded_width) {
        av_log(avctx, AV_LOG_WARNING, "Iris compressed capture not used: the SPS crops %dx%d to %dx%d, "
               "whose compressed layout differs\n",
               s->q08c_crop_coded_width, s->q08c_crop_coded_height, s->q08c_crop_width, s->q08c_crop_height);
        priv->capture_ubwc = 0;
    }

    xlat_init(&s->xlat);
    pts_stats_init(&s->pts_stat, avctx, "decoder");

    capture = &s->capture;
    output = &s->output;

    /* if these dimensions are invalid (ie, 0 or too small) an event will be raised
     * by the v4l2 driver; this event will trigger a full pipeline reconfig and
     * the proper values will be retrieved from the kernel driver.
     */
    // Iris requires the known coded size before allocating OUTPUT buffers.
    output->height = capture->height = avctx->coded_height;
    output->width = capture->width = avctx->coded_width;

    output->av_codec_id = avctx->codec_id;
    output->av_pix_fmt  = AV_PIX_FMT_NONE;
    output->min_buf_size = max_coded_size(avctx);

    capture->av_codec_id = AV_CODEC_ID_RAWVIDEO;
    capture->av_pix_fmt = avctx->pix_fmt;
    capture->min_buf_size = 0;

    capture->av_pix_fmt = AV_PIX_FMT_NONE;
    s->output_drm = 0;

    s->db_ctl = NULL;
    if (priv->dmabuf_alloc != NULL && strcmp(priv->dmabuf_alloc, "v4l2") != 0) {
        if (strcmp(priv->dmabuf_alloc, "cma") == 0)
            s->db_ctl = dmabufs_ctl_new();
        else {
            av_log(avctx, AV_LOG_ERROR, "Unknown dmabuf alloc method: '%s'\n", priv->dmabuf_alloc);
            return AVERROR(EINVAL);
        }
        if (!s->db_ctl) {
            av_log(avctx, AV_LOG_ERROR, "Can't open dmabuf provider '%s'\n", priv->dmabuf_alloc);
            return AVERROR(ENOMEM);
        }
    }

    s->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!s->device_ref) {
        ret = AVERROR(ENOMEM);
        return ret;
    }

    ret = av_hwdevice_ctx_init(s->device_ref);
    if (ret < 0)
        return ret;

    s->avctx = avctx;
    ret = ff_v4l2_m2m_codec_init(priv);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "can't configure decoder\n");
        return ret;
    }

    if (avctx->extradata &&
        (ret = copy_extradata(avctx, avctx->extradata, avctx->extradata_size, &s->extdata_data, &s->extdata_size)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to copy extradata from context: %s\n", av_err2str(ret));
#if DUMP_FAILED_EXTRADATA
        log_dump(avctx, AV_LOG_INFO, avctx->extradata, avctx->extradata_size);
#endif
        return ret;
    }

    if ((ret = get_quirks(avctx, s)) != 0)
        return ret;

    if ((ret = check_profile(avctx, s)) != 0) {
        av_log(avctx, AV_LOG_WARNING, "Profile %d not supported by decode\n", avctx->profile);
        return ret;
    }

    // Size check done as part of format filtering
    if ((ret = choose_capture_format(avctx, s)) != 0)
        return ret;

    if ((ret = v4l2_prepare_decoder(s)) < 0)
        return ret;

    return 0;
}

static av_cold int v4l2_decode_close(AVCodecContext *avctx)
{
    int rv;
    av_log(avctx, AV_LOG_TRACE, "<<< %s\n", __func__);
    rv = ff_v4l2_m2m_codec_end(avctx->priv_data);
    av_log(avctx, AV_LOG_TRACE, ">>> %s: rv=%d\n", __func__, rv);
    return rv;
}

// Keep Iris's admitted session and capture pool. Restarting either queue
// repeats admission at the measured input rate, which can refuse even the
// same video. Drain old pictures through LAST, then restart without STREAMOFF.
static int iris_drain_seek(AVCodecContext *avctx, V4L2m2mContext *s)
{
    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
    const int64_t began = av_gettime_relative();
    const int64_t deadline = began + 500000;
    AVFrame *frame;
    int ret = 0;

    av_packet_unref(&s->buf_pkt);
    if (!s->output.streamon && !s->capture.streamon && !s->running &&
        (s->iris_startup == IRIS_START_OFF || s->iris_startup == IRIS_START_FIRST_PACKET)) {
        xlat_flush(&s->xlat);
        pts_stats_init(&s->pts_stat, avctx, "decoder");
        s->extdata_sent = s->req_pkt = 0;
        return 0;
    }
    if (s->iris_startup && s->iris_startup != IRIS_START_READY)
        return AVERROR(EAGAIN);
    if (!s->output.streamon || !s->capture.streamon)
        return AVERROR(EAGAIN);

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);
    s->iris_seek_deadline = deadline;
    if (!s->capture.flag_last && !s->draining) {
        while (ioctl(s->fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
            if (av_gettime_relative() >= deadline) {
                ret = AVERROR(ETIMEDOUT);
                goto done;
            }
            if (errno != EINTR) {
                ret = AVERROR(errno);
                goto done;
            }
        }
    }
    s->draining = 1;
    while (!s->capture.flag_last && av_gettime_relative() < deadline) {
        ret = ff_v4l2_context_dequeue_frame(&s->capture, frame, 10);
        av_frame_unref(frame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF &&
            ret != AVERROR_INVALIDDATA && ret != AVERROR(ENOSPC))
            goto done;
    }
    if (!s->capture.flag_last || !s->capture.actual_last) {
        ret = AVERROR(ETIMEDOUT);
        goto done;
    }

    // Consume the EOS notification before resuming. Iris may have only LAST,
    // with no EOS event. No queued source-change event is discarded here.
    if (!s->capture.done) {
        ret = ff_v4l2_context_dequeue_frame(&s->capture, frame, 0);
        av_frame_unref(frame);
        if (ret != AVERROR_EOF) {
            ret = ret < 0 ? ret : AVERROR(EIO);
            goto done;
        }
    }
    cmd.cmd = V4L2_DEC_CMD_START;
    while (ioctl(s->fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
        if (av_gettime_relative() >= deadline) {
            ret = AVERROR(ETIMEDOUT);
            goto done;
        }
        if (errno != EINTR) {
            ret = AVERROR(errno);
            goto done;
        }
    }
    xlat_flush(&s->xlat);
    pts_stats_init(&s->pts_stat, avctx, "decoder");
    s->extdata_sent = 0;
    s->running = s->draining = s->req_pkt = 0;
    s->output.done = s->capture.done = 0;
    s->capture.flag_last = 0;
    s->capture.actual_last = 0;
    ret = 0;
    av_log(avctx, AV_LOG_VERBOSE, "Iris seek kept the decoder session (%.1f ms)\n",
           (av_gettime_relative() - began) / 1000.0);
done:
    s->iris_seek_deadline = 0;
    av_frame_free(&frame);
    return ret;
}

static void v4l2_decode_flush(AVCodecContext *avctx)
{
    // An alternatve and more drastic form of flush is to simply do this:
    //    v4l2_decode_close(avctx);
    //    v4l2_decode_init(avctx);
    // The downside is that this keeps a decoder open until all the frames
    // associated with it have been returned.  This is a bit wasteful on
    // possibly limited h/w resources and fails on a Pi for this reason unless
    // more GPU mem is allocated than is the default.

    V4L2m2mPriv * const priv = avctx->priv_data;
    V4L2m2mContext * const s = priv->context;
    V4L2Context * const output = &s->output;
    V4L2Context * const capture = &s->capture;

    priv->drain_seek_status = 0;
    if (s->quirks & FF_V4L2_QUIRK_IRIS_DRAIN_SEEK) {
        int ret = iris_drain_seek(avctx, s);
        priv->drain_seek_status = ret < 0 ? ret : 1;
        if (ret < 0)
            av_log(avctx, AV_LOG_WARNING, "Iris seek drain failed: %s; reopen needed\n", av_err2str(ret));
        return;
    }

    av_log(avctx, AV_LOG_TRACE, "<<< %s: streamon=%d\n", __func__, output->streamon);

    // Reflushing everything is benign, quick and avoids having to worry about
    // states like EOS processing so don't try to optimize out (having got it
    // wrong once)

    ff_v4l2_context_set_status(output, VIDIOC_STREAMOFF);

    // Clear any buffered input packet
    av_packet_unref(&s->buf_pkt);

    // Clear a pending EOS
    if (ff_v4l2_ctx_eos(capture)) {
        // Arguably we could delay this but this is easy and doesn't require
        // thought or extra vars
        ff_v4l2_context_set_status(capture, VIDIOC_STREAMOFF);
        ff_v4l2_context_set_status(capture, VIDIOC_STREAMON);
    }

    // V4L2 makes no guarantees about whether decoded frames are flushed or not
    // so mark all frames we are tracking to be discarded if they appear
    xlat_flush(&s->xlat);

    // resend extradata
    s->extdata_sent = 0;
    // clear status vars
    s->running = 0;
    s->draining = 0;
    output->done = 0;
    capture->done = 0;

    // Stream on will occur when we actually submit a new frame
    av_log(avctx, AV_LOG_TRACE, ">>> %s\n", __func__);
}

#define OFFSET(x) offsetof(V4L2m2mPriv, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    V4L_M2M_DEFAULT_OPTS,
    { "num_capture_buffers", "Number of buffers in the capture context",
        OFFSET(num_capture_buffers), AV_OPT_TYPE_INT, {.i64 = 20}, 2, INT_MAX, FLAGS },
    { "capture_spare", "Capture buffers beyond the driver's minimum; replaces num_capture_buffers (-1: off)",
        OFFSET(capture_spare), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 32, FLAGS },
    { "drain_seek_status", "Result of the last same-session seek (1: kept, negative: reopen needed)",
        OFFSET(drain_seek_status), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, 1,
        FLAGS | AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    { "capture_ubwc", "Use Qualcomm compressed NV12 capture for DRM output",
        OFFSET(capture_ubwc), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { "pixel_format", "Pixel format to be used by the decoder", OFFSET(pix_fmt), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_NONE}, AV_PIX_FMT_NONE, AV_PIX_FMT_NB, FLAGS },
    { "dmabuf_alloc", "Dmabuf alloc method", OFFSET(dmabuf_alloc), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { NULL},
};

static const AVCodecHWConfigInternal *v4l2_m2m_hw_configs[] = {
    HW_CONFIG_INTERNAL(DRM_PRIME),
    NULL
};

#define M2MDEC_CLASS(NAME) \
    static const AVClass v4l2_m2m_ ## NAME ## _dec_class = { \
        .class_name = #NAME "_v4l2m2m_decoder", \
        .item_name  = av_default_item_name, \
        .option     = options, \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define M2MDEC(NAME, LONGNAME, CODEC, bsf_name) \
    M2MDEC_CLASS(NAME) \
    const FFCodec ff_ ## NAME ## _v4l2m2m_decoder = { \
        .p.name         = #NAME "_v4l2m2m" , \
        CODEC_LONG_NAME("V4L2 mem2mem " LONGNAME " decoder wrapper"), \
        .p.type         = AVMEDIA_TYPE_VIDEO, \
        .p.id           = CODEC , \
        .priv_data_size = sizeof(V4L2m2mPriv), \
        .p.priv_class   = &v4l2_m2m_ ## NAME ## _dec_class, \
        .init           = v4l2_decode_init, \
        FF_CODEC_RECEIVE_FRAME_CB(v4l2_receive_frame), \
        .close          = v4l2_decode_close, \
        .flush          = v4l2_decode_flush, \
        .bsfs           = bsf_name, \
        .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING, \
        .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | \
                          FF_CODEC_CAP_INIT_CLEANUP, \
        .p.wrapper_name = "v4l2m2m", \
        CODEC_PIXFMTS(AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P), \
        .hw_configs     = v4l2_m2m_hw_configs, \
    }

M2MDEC(h264,  "H.264", AV_CODEC_ID_H264,       "h264_mp4toannexb");
M2MDEC(hevc,  "HEVC",  AV_CODEC_ID_HEVC,       "hevc_mp4toannexb");
M2MDEC(mpeg1, "MPEG1", AV_CODEC_ID_MPEG1VIDEO, NULL);
M2MDEC(mpeg2, "MPEG2", AV_CODEC_ID_MPEG2VIDEO, NULL);
M2MDEC(mpeg4, "MPEG4", AV_CODEC_ID_MPEG4,      NULL);
M2MDEC(h263,  "H.263", AV_CODEC_ID_H263,       NULL);
M2MDEC(vc1 ,  "VC1",   AV_CODEC_ID_VC1,        NULL);
M2MDEC(vp8,   "VP8",   AV_CODEC_ID_VP8,        NULL);
M2MDEC(vp9,   "VP9",   AV_CODEC_ID_VP9,        NULL);
