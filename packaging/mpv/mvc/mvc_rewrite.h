/* Two-view H.264 MVC (3D Blu-ray, Stereo High) rewritten as plain H.264.
 *
 * Each access unit of a two-view MVC stream becomes ordinary H.264 pictures
 * in decoding order: the base view's picture, then sometimes a hidden copy of
 * it, then the dependent view's picture. Any H.264 decoder then decodes, as
 * one frame-sequential stream, exactly the pictures an MVC decoder gives for
 * both views (ITU-T H.264 Annex H in editions before 2024, Annex G since).
 * Slice data is copied unchanged; only parameter sets and slice headers are
 * rewritten: one SPS for both views, reference lists written out explicitly,
 * reference marking done with explicit commands, and frame_num and picture
 * order count numbered as one stream.
 *
 * Input: one access unit per call holding both views' NAL units (base view
 * first or not), Annex B or length-prefixed, as an MKV block or a transport
 * stream's two views joined by DTS give it. NAL units the rewrite doesn't
 * need (SEI, delimiters, Blu-ray's type 24, end of sequence) are dropped.
 *
 * Output: pictures in decoding order, each in Annex B with an access unit
 * delimiter first and, before an IDR, the SPS and PPSs it needs. The decoder
 * needs no extradata: open it without any and start it on the first picture
 * with idr set (nothing comes before one). Feed each picture to the decoder
 * as a packet of its own.
 *
 * Holding: the first access units after each IDR can be held back, usually
 * one to three (the period's first B picture settles it), at most 16; they
 * come out with a later call, all at once. Each picture carries the tag its
 * access unit was given (a packet's timestamp, say), so the caller can time
 * it. At the end of the stream, mvc_rewriter_flush returns what is held.
 *
 * Timing and pairing after the decoder: every picture comes out of the
 * decoder, hidden ones too, in display order with each access unit's base,
 * hidden and dependent picture next to each other (picture order counts are
 * P and P + 1, or 4P, 4P + 1 and 4P + 2, where P is the access unit's order
 * count relative to its IDR). The decoder's reorder depth is in the SPS (VUI
 * bitstream_restriction), so a decoder that honours it never outputs out of
 * order. Decoders don't report the order count, so pass each packet's tag
 * and slot through the decoder (with FFmpeg: AVPacket.opaque and
 * AV_CODEC_FLAG_COPY_OPAQUE), join the base and dependent frames that carry
 * the same tag, and drop hidden ones. A pair shows at its access unit's time;
 * the rewritten stream itself carries no timing. Frame dropping in the
 * decoder must stay off: every picture may be a reference.
 *
 * Unsupported streams (the second view predicting from a third, field or
 * MBAFF coding, long-term references, MMCO 5, slice groups, redundant
 * pictures, SP/SI slices, data partitioning, separate colour planes, views
 * that differ in size, chroma format or bit depth, temporal direct or
 * implicit weighted prediction together with odd order counts or a hidden
 * copy in one IDR period) make mvc_rewrite return MVC_UNSUPPORTED from then
 * on: play the base view as 2D (an H.264 decoder ignores the dependent
 * view's NAL units). A damaged access unit (unparsable header, missing view
 * or picture, frame_num gap, lost reference) returns MVC_DROPPED, as does
 * everything after it until the next IDR access unit.
 *
 * Cost, measured on a 1080p 3D Blu-ray on one desktop core (i7-12700K):
 * 30 to 50 us per access unit (median), under 0.15 ms for 99%, 0.3 ms at
 * most. It grows with the access unit's size: slice data is copied, not
 * parsed.
 *
 * One rewriter per stream; instances share nothing, and no call blocks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mvc_slot {
    MVC_SLOT_BASE,
    MVC_SLOT_DEPENDENT,
    MVC_SLOT_HIDDEN,  // decoded as a reference, never shown
};

struct mvc_picture {
    enum mvc_slot slot;
    const uint8_t *data;  // Annex B, valid until the next call on the rewriter
    size_t size;
    bool idr;             // a decoder may start here
    int32_t poc;          // order count as written, within the IDR period
    int64_t tag;          // as given with the picture's access unit
};

enum mvc_status {
    MVC_OK,           // the access unit was taken
    MVC_DROPPED,      // it was dropped: damaged, or waiting for an IDR
    MVC_UNSUPPORTED,  // can't be rewritten: play the base view in 2D
    MVC_NO_MEMORY,    // it was dropped; the next IDR resyncs
};

struct mvc_rewriter;

// codec_private: an avcC record, optionally followed by an mvcC box
// (Matroska CodecPrivate as written for MVC), makes the input
// length-prefixed with avcC's length size. Otherwise (NULL, or Annex B
// parameter sets) the input is Annex B. Returns NULL when out of memory.
struct mvc_rewriter *mvc_rewriter_create(const uint8_t *codec_private,
                                         size_t size);
void mvc_rewriter_destroy(struct mvc_rewriter *r);

// Rewrites one access unit. *out receives the pictures ready now, *count
// how many: this access unit's, earlier held ones', or none while it is
// held. Pictures can come with any status but MVC_UNSUPPORTED (those
// written before damage stay valid). An access unit holding only parameter
// sets is taken with no picture.
enum mvc_status mvc_rewrite(struct mvc_rewriter *r, const uint8_t *data,
                            size_t size, int64_t tag,
                            const struct mvc_picture **out, int *count);

// End of stream: returns the pictures still held.
void mvc_rewriter_flush(struct mvc_rewriter *r, const struct mvc_picture **out,
                        int *count);

// Seek: forget all pictures, held ones too, and wait for the next IDR access
// unit. Parameter sets already seen are kept.
void mvc_rewriter_reset(struct mvc_rewriter *r);

// Why the last call returned MVC_UNSUPPORTED or MVC_DROPPED (static text).
const char *mvc_rewriter_reason(const struct mvc_rewriter *r);
