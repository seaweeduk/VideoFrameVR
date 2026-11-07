/* Internals of the MVC rewrite: bit reading and writing on RBSPs, and the
 * parsed form of parameter sets and slice headers. Clause numbers are those
 * of ITU-T H.264 (06/2026), where MVC is Annex G.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reader over an RBSP (emulation prevention bytes removed). Reading past the
// end yields zeros and sets overrun, so callers check once per structure.
struct bitreader {
    const uint8_t *data;
    size_t bits;
    size_t pos;
    bool overrun;
};

static inline void br_init(struct bitreader *br, const uint8_t *data, size_t size)
{
    *br = (struct bitreader){.data = data, .bits = size * 8};
}

static inline unsigned br_u1(struct bitreader *br)
{
    if (br->pos >= br->bits) {
        br->overrun = true;
        return 0;
    }
    unsigned v = (br->data[br->pos >> 3] >> (7 - (br->pos & 7))) & 1;
    br->pos++;
    return v;
}

static inline uint32_t br_u(struct bitreader *br, int n)
{
    uint32_t v = 0;
    if (!n)
        return 0;
    if (br->pos + n <= br->bits && n <= 25) {
        // Up to 25 bits fit in 4 bytes from any bit offset.
        size_t byte = br->pos >> 3;
        uint32_t w = 0;
        for (int i = 0; i < 4; i++)
            w = (w << 8) | (byte + i < (br->bits >> 3) ? br->data[byte + i] : 0);
        v = (w << (br->pos & 7)) >> (32 - n);
        br->pos += n;
        return v;
    }
    for (int i = 0; i < n; i++)
        v = (v << 1) | br_u1(br);
    return v;
}

static inline uint32_t br_ue(struct bitreader *br)
{
    int zeros = 0;
    while (!br_u1(br)) {
        if (++zeros > 31 || br->overrun) {
            br->overrun = true;
            return 0;
        }
    }
    if (!zeros)
        return 0;
    return (uint32_t)(((uint64_t)1 << zeros) - 1 + br_u(br, zeros));
}

static inline int32_t br_se(struct bitreader *br)
{
    uint32_t k = br_ue(br);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

// Growable bit writer. Allocation failure sets failed; callers check once.
struct bitwriter {
    uint8_t *data;
    size_t cap;   // bytes
    size_t pos;   // bits
    bool failed;
};

bool bw_reserve(struct bitwriter *bw, size_t bytes);
void bw_u(struct bitwriter *bw, int n, uint32_t v);
void bw_ue(struct bitwriter *bw, uint32_t v);
void bw_se(struct bitwriter *bw, int32_t v);
// Copies nbits starting at bit start of src.
void bw_copy(struct bitwriter *bw, const uint8_t *src, size_t start, size_t nbits);
// rbsp_trailing_bits(): a one and zeros up to the byte boundary.
void bw_trailing(struct bitwriter *bw);

#define MAX_VIEWS_KEPT 2
#define MAX_REF_FRAMES 16
#define MAX_LIST 32
#define MAX_MODS 33
#define MAX_MMCO 40

// Scaling lists in coding order: 4x4 Intra Y, Cb, Cr, Inter Y, Cb, Cr, then
// 8x8 Intra Y, Inter Y (4:2:0 and 4:2:2).
struct scaling {
    uint8_t l4[6][16];
    uint8_t l8[2][64];
};

struct sps {
    bool valid;
    bool subset;
    uint8_t *rbsp;  // own copy, to detect changes
    size_t rbsp_size;

    int profile_idc, constraint_flags, level_idc, id;
    // chroma_format_idc up to the end of the scaling lists, for copying
    // and for comparing the views' coding tools.
    size_t tools_start, tools_end;
    bool high;  // the profile carries those fields
    int chroma_format_idc;
    bool separate_colour_plane;
    int bit_depth_luma, bit_depth_chroma;
    bool lossless, scaling_matrix;
    struct scaling lists;  // in effect, after fall-back rule A

    int log2_max_frame_num;
    int poc_type;
    int log2_max_poc_lsb;
    bool delta_pic_order_always_zero;
    int offset_for_non_ref_pic, offset_for_top_to_bottom_field;
    int num_ref_frames_in_poc_cycle;
    int32_t offset_for_ref_frame[256];
    int32_t expected_delta_per_poc_cycle;
    int max_num_ref_frames;
    bool gaps_allowed;
    int width_mbs, height_mbs;
    bool frame_mbs_only, mbaff, direct_8x8_inference;
    bool cropping;
    uint32_t crop[4];

    bool vui;
    bool aspect_present;
    int aspect_idc, sar_w, sar_h;
    bool overscan_present, overscan_appropriate;
    bool signal_present;
    int video_format;
    bool full_range, colour_present;
    int primaries, transfer, matrix;
    bool chroma_loc_present;
    uint32_t chroma_loc_top, chroma_loc_bottom;
    bool restriction;
    bool mv_over_boundaries;
    // As coded (copied to the rewritten SPS; activate() checks the last two)
    uint32_t max_bytes_per_pic_denom, max_bits_per_mb_denom;
    uint32_t log2_max_mv_h, log2_max_mv_v;
    uint32_t num_reorder_frames, max_dec_frame_buffering;

    // Subset SPS, MVC extension: views in view order, and the inter-view
    // references of view order index 1.
    int num_views;
    int view_id[MAX_VIEWS_KEPT];
    int refs_count[2][2];  // [anchor][list]
    int refs[2][2][16];
};

struct pps {
    bool valid;
    uint8_t *rbsp;
    size_t rbsp_size;
    size_t rest_start;  // first bit after seq_parameter_set_id
    size_t stop_bit;    // position of rbsp_stop_one_bit
    int id, sps_id;
    bool cabac, bottom_field_pic_order;
    int num_slice_groups;
    int num_ref_idx_default[2];
    bool weighted_pred;
    int weighted_bipred_idc;
    bool deblocking_control, redundant_pic_cnt;
    int chroma_qp_index_offset;
    // Fields after redundant_pic_cnt_present_flag, when present
    size_t ext_start;
    bool ext, ext_ok;
    bool transform_8x8, pic_scaling_matrix;
    int second_chroma_qp_index_offset;
    bool list_present[8], list_default[8];
    struct scaling raw;
};

enum { SLICE_P = 0, SLICE_B = 1, SLICE_I = 2, SLICE_SP = 3, SLICE_SI = 4 };

struct mmco {
    int op;
    uint32_t a, b;
};

struct pic;

struct slice {
    const uint8_t *rbsp;
    size_t rbsp_size;
    int nal_ref_idc;
    bool idr;
    // MVC header (prefix NAL for the base view)
    int view_id;
    bool anchor, inter_view;

    int first_mb, raw_type, type, pps_id;
    const struct pps *pps;
    const struct sps *sps;
    int frame_num, idr_pic_id;
    int poc_lsb, delta_poc_bottom, delta_poc[2];
    bool direct_spatial;
    int num_ref_idx[2];
    int nmods[2];
    struct {
        int idc;
        uint32_t val;
    } mods[2][MAX_MODS];
    size_t wpt_start, wpt_end;  // pred_weight_table() bits, copied as they are
    bool no_output_of_prior_pics, long_term_reference, adaptive;
    int nmmco;
    struct mmco mmco[MAX_MMCO];
    size_t tail_start, header_end;  // cabac_init_idc .. end of the header
    size_t stop_bit;

    // Reference lists as Annex G builds them; NULL is "no reference picture".
    struct pic *list[2][MAX_LIST + 1];
};

// Parsing returns NULL on success, else the reason.
const char *parse_sps(struct sps *sps, const uint8_t *rbsp, size_t size, bool subset);
const char *parse_pps(struct pps *pps, const uint8_t *rbsp, size_t size);
// sps_table: SPSs for the base view, subset SPSs for the dependent view.
const char *parse_slice_header(struct slice *s, const uint8_t *rbsp, size_t size,
                               int nal_type, int nal_ref_idc,
                               struct pps *const *pps_table,
                               struct sps *const *sps_table);
size_t find_stop_bit(const uint8_t *rbsp, size_t size);
// Scaling lists a picture uses with this PPS and SPS (Table 7-2).
void pps_scaling(const struct pps *pps, const struct sps *sps, struct scaling *out);
void write_scaling(struct bitwriter *bw, const struct scaling *lists, bool transform_8x8);
