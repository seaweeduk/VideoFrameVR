/* Picture subtitles (Blu-ray PGS, DVD VobSub, DVB) handed to the app instead
 * of drawn into the video. Shared, as C, by the app and
 * packaging/mpv/mpv-subtitle-export.patch: the app passes a struct
 * subtitle_export as render parameter SUBTITLE_EXPORT_PARAM with each
 * mpv_render_context_render(). Stock mpv ignores the parameter and leaves
 * handled at 0. */
#pragma once

#include <stdint.h>

#define SUBTITLE_EXPORT_PARAM 0x7f01
#define SUBTITLE_EXPORT_MAGIC 0x53554231u /* "SUB1" */

struct subtitle_export_part {
    const uint8_t *bitmap; /* BGRA, premultiplied alpha, rows of stride bytes */
    int32_t stride;
    int32_t w, h;          /* the bitmap's size */
    int32_t x, y, dw, dh;  /* where it goes on the canvas, scaled to dw × dh */
};

struct subtitle_export {
    /* In, set by the app. */
    uint32_t magic; /* SUBTITLE_EXPORT_MAGIC */
    uint32_t size;  /* sizeof(struct subtitle_export) */
    /* 1: the app draws the subtitles: mpv no longer draws them into the
     * video and fills in the parts below. 0: mpv draws them as usual. */
    int32_t take;
    /* The canvas the parts are laid out on: one eye's picture as shown, in
     * square pixels. */
    int32_t canvas_w, canvas_h;

    /* Out, set by mpv on every call. */
    int32_t handled;   /* 1: this mpv knows the parameter */
    /* Changes whenever the parts do; the same: nothing to copy again. */
    int64_t change_id;
    int32_t num_parts; /* 0: no subtitle showing */
    /* num_parts of them; they and their bitmaps stay valid until the next
     * render call. */
    const struct subtitle_export_part *parts;
};
