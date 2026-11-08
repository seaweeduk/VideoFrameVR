/* Handing decoded frames from the Frame's private mpv to the app instead of
 * drawing them (docs/vulkan-renderer.md, section 11). Shared, as C, by the
 * app and packaging/frame/mpv-v4l2-reset.patch: the app passes a struct
 * video_export as render parameter VIDEO_EXPORT_PARAM with each
 * mpv_render_context_render(). Stock mpv ignores the parameter and draws.
 * Passed to mpv_render_context_set_parameter() instead, mpv only takes its
 * releases (packaging/frame/mpv-export-release.patch); stock mpv refuses it. */
#pragma once

#include <stdint.h>

#define VIDEO_EXPORT_PARAM 0x7f00
#define VIDEO_EXPORT_MAGIC 0x56584631u /* "VXF1" */
#define VIDEO_EXPORT_MAX_RELEASE 16

enum video_export_matrix {
    VIDEO_EXPORT_MATRIX_OTHER = 0,
    VIDEO_EXPORT_MATRIX_BT601 = 1,
    VIDEO_EXPORT_MATRIX_BT709 = 2,
    VIDEO_EXPORT_MATRIX_BT2020 = 3, /* non-constant luminance */
};

enum video_export_chroma {
    VIDEO_EXPORT_CHROMA_LEFT = 0,   /* MPEG-2/4: co-sited horizontally, centred vertically */
    VIDEO_EXPORT_CHROMA_CENTER = 1, /* MPEG-1/JPEG */
    VIDEO_EXPORT_CHROMA_TOP_LEFT = 2,
    VIDEO_EXPORT_CHROMA_OTHER = 3,
};

struct video_export {
    /* In, set by the app. */
    uint32_t magic;   /* VIDEO_EXPORT_MAGIC */
    uint32_t size;    /* sizeof(struct video_export) */
    /* 1: hand out the frame instead of drawing it; if it cannot be (exported
     * -1), nothing is drawn: the app calls again without request. */
    int32_t request;
    int32_t release_count;  /* frames (ids) the app is done with; mpv frees them first */
    uint64_t release[VIDEO_EXPORT_MAX_RELEASE];
    /* 1: the app samples Q10C (P010 with DRM_FORMAT_MOD_QCOM_COMPRESSED)
     * decoder buffers itself: they are handed out as the decoder wrote them,
     * without a conversion. */
    int32_t accept_q10c;
    /* 1: the same for Q08C (NV12 with DRM_FORMAT_MOD_QCOM_COMPRESSED), which
     * the decoder writes when mpv's hwdec-v4l2-q08c is on as it opens. */
    int32_t accept_q08c;

    /* Out, set by mpv on every call. */
    int32_t exported;  /* 0: drawn as usual, -1: not handed out, 1: a new frame, 2: frame id again */
    int32_t fence_fd;  /* new frame: its conversion's completion (sync_file), the app's to close; -1: done */
    uint64_t id;       /* unique for this render backend's lifetime, across converter replacements */
    int32_t fd;        /* the buffer's DMA-BUF, mpv's (dup it to keep it) */
    uint32_t drm_format; /* DRM_FORMAT_P010 or DRM_FORMAT_NV12 */
    uint64_t modifier;   /* DRM_FORMAT_MOD_LINEAR, or QCOM_COMPRESSED (accept_q10c, accept_q08c) */
    uint64_t buffer_size;
    uint32_t offset[2], pitch[2]; /* luma, interleaved chroma */
    int32_t width, height;
    int32_t matrix;       /* video_export_matrix */
    int32_t full_range;   /* 0: limited (TV), 1: full (PC) */
    int32_t chroma;       /* video_export_chroma */
    int32_t bt709_primaries; /* 1: BT.709 (as mpv's target), 0: other */
    int32_t sdr;          /* 1: an SDR transfer, not linear */

    /* Once mpv has handed out a decoder's own buffers (linear NV12, Q10C, Q08C), its
     * decoder thread adds 1 to session whenever a decoder closes, then waits
     * (at most 100 ms) until the app, having given back every frame it does
     * not show (release, also accepted with MPV_RENDER_PARAM_SKIP_RENDERING)
     * and let go of every import it no longer samples, sets session_done to
     * it: the next decoder session can only allocate its buffers once the old
     * ones are freed, and each frame keeps its session alive. Both atomic. */
    uint32_t session;
    uint32_t session_done;
    /* Sleep closure, pumped with SKIP_RENDERING even without XR frames.
     * close_pending is the app's; buffers_released is mpv's acknowledgement
     * that converter inputs and exported/drawn frames have all gone back. */
    int32_t close_pending;
    int32_t buffers_released;
};
