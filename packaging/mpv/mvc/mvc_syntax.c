/* Parameter set and slice header syntax for the MVC rewrite (clause 7.3,
 * with the MVC additions of G.7.3), and the bit writer.
 */
#include <stdlib.h>
#include <string.h>
#include "mvc_internal.h"

bool bw_reserve(struct bitwriter *bw, size_t bytes)
{
    size_t need = (bw->pos + 7) / 8 + bytes + 8;
    if (need <= bw->cap)
        return !bw->failed;
    size_t cap = bw->cap ? bw->cap : 256;
    while (cap < need)
        cap *= 2;
    uint8_t *data = realloc(bw->data, cap);
    if (!data) {
        bw->failed = true;
        return false;
    }
    memset(data + bw->cap, 0, cap - bw->cap);
    bw->data = data;
    bw->cap = cap;
    return true;
}

void bw_u(struct bitwriter *bw, int n, uint32_t v)
{
    if (!n || !bw_reserve(bw, 5))
        return;
    for (int i = n - 1; i >= 0; i--) {
        size_t byte = bw->pos >> 3;
        int shift = 7 - (bw->pos & 7);
        if (shift == 7)
            bw->data[byte] = 0;
        bw->data[byte] |= ((v >> i) & 1) << shift;
        bw->pos++;
    }
}

void bw_ue(struct bitwriter *bw, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1;
    int len = 0;
    while ((x >> len) > 1)
        len++;
    bw_u(bw, len, 0);
    if (len >= 32) {
        bw_u(bw, 1, 1);
        bw_u(bw, len, (uint32_t)x);
    } else {
        bw_u(bw, len + 1, (uint32_t)x);
    }
}

void bw_se(struct bitwriter *bw, int32_t v)
{
    bw_ue(bw, v > 0 ? 2 * (uint32_t)v - 1 : 2 * (uint32_t)(-(int64_t)v));
}

void bw_copy(struct bitwriter *bw, const uint8_t *src, size_t start, size_t nbits)
{
    if (!bw_reserve(bw, nbits / 8 + 2))
        return;
    // Bring the writer to a byte boundary, then copy whole bytes.
    while (nbits && (bw->pos & 7)) {
        bw_u(bw, 1, (src[start >> 3] >> (7 - (start & 7))) & 1);
        start++;
        nbits--;
    }
    size_t bytes = nbits / 8;
    uint8_t *dst = bw->data + bw->pos / 8;
    const uint8_t *s = src + start / 8;
    int shift = start & 7;
    if (!shift) {
        memcpy(dst, s, bytes);
    } else {
        for (size_t i = 0; i < bytes; i++)
            dst[i] = (uint8_t)((s[i] << shift) | (s[i + 1] >> (8 - shift)));
    }
    bw->pos += bytes * 8;
    start += bytes * 8;
    nbits -= bytes * 8;
    while (nbits--) {
        bw_u(bw, 1, (src[start >> 3] >> (7 - (start & 7))) & 1);
        start++;
    }
}

void bw_trailing(struct bitwriter *bw)
{
    bw_u(bw, 1, 1);
    if (bw->pos & 7)
        bw_u(bw, 8 - (bw->pos & 7), 0);
}

size_t find_stop_bit(const uint8_t *rbsp, size_t size)
{
    while (size && !rbsp[size - 1])
        size--;
    if (!size)
        return 0;
    uint8_t last = rbsp[size - 1];
    int bit = 0;
    while (!((last >> bit) & 1))
        bit++;
    return size * 8 - 1 - bit;
}

static const uint8_t default_4x4[2][16] = {
    {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42},
    {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34},
};

static const uint8_t default_8x8[2][64] = {
    {6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
     23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
     27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
     31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42},
    {9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
     21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
     24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
     27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35},
};

// scaling_list() (7.3.2.1.1.1). Returns false on a bad delta.
static bool read_scaling_list(struct bitreader *br, uint8_t *list, int size,
                              bool *use_default)
{
    int last = 8, next = 8;
    *use_default = false;
    for (int j = 0; j < size; j++) {
        if (next) {
            int32_t delta = br_se(br);
            if (delta < -128 || delta > 127)
                return false;
            next = (last + delta + 256) % 256;
            *use_default = !j && !next;
        }
        list[j] = next ? next : last;
        last = list[j];
    }
    return true;
}

static uint8_t *list_at(struct scaling *s, int i)
{
    return i < 6 ? s->l4[i] : s->l8[i - 6];
}

static const uint8_t *default_list(int i)
{
    return i < 6 ? default_4x4[i >= 3] : default_8x8[i - 6];
}

// Fall-back for a list that isn't coded: rule A takes defaults, rule B the
// sequence-level list, for i = 0, 3, 6, 7; the others repeat the list before.
static void fall_back(struct scaling *out, int i, const struct scaling *seq)
{
    int size = i < 6 ? 16 : 64;
    if (i == 0 || i == 3 || i >= 6)
        memcpy(list_at(out, i), seq ? list_at((struct scaling *)seq, i) : default_list(i), size);
    else
        memcpy(list_at(out, i), list_at(out, i - 1), size);
}

static void flat(struct scaling *s)
{
    memset(s, 16, sizeof(*s));
}

void pps_scaling(const struct pps *pps, const struct sps *sps, struct scaling *out)
{
    if (!pps->pic_scaling_matrix) {
        *out = sps->lists;
        return;
    }
    for (int i = 0; i < 8; i++) {
        if (i >= 6 && !pps->transform_8x8)
            memcpy(out->l8[i - 6], sps->lists.l8[i - 6], 64);
        else if (!pps->list_present[i])
            fall_back(out, i, sps->scaling_matrix ? &sps->lists : NULL);
        else if (pps->list_default[i])
            memcpy(list_at(out, i), default_list(i), i < 6 ? 16 : 64);
        else
            memcpy(list_at(out, i), list_at((struct scaling *)&pps->raw, i), i < 6 ? 16 : 64);
    }
}

void write_scaling(struct bitwriter *bw, const struct scaling *lists, bool transform_8x8)
{
    for (int i = 0; i < (transform_8x8 ? 8 : 6); i++) {
        const uint8_t *l = list_at((struct scaling *)lists, i);
        int last = 8;
        bw_u(bw, 1, 1);
        for (int j = 0; j < (i < 6 ? 16 : 64); j++) {
            int delta = l[j] - last;
            if (delta > 127)
                delta -= 256;
            else if (delta < -128)
                delta += 256;
            bw_se(bw, delta);
            last = l[j];
        }
    }
}

static bool skip_hrd(struct bitreader *br)
{
    uint32_t count = br_ue(br) + 1;
    if (count > 32)
        return false;
    br_u(br, 8);
    for (uint32_t i = 0; i < count; i++) {
        br_ue(br);
        br_ue(br);
        br_u1(br);
    }
    br_u(br, 20);
    return true;
}

static bool high_profile(int p)
{
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 ||
           p == 86 || p == 118 || p == 128 || p == 138 || p == 139 ||
           p == 134 || p == 135;
}

const char *parse_sps(struct sps *sps, const uint8_t *rbsp, size_t size, bool subset)
{
    struct bitreader b, *br = &b;
    br_init(br, rbsp, size);
    memset(sps, 0, sizeof(*sps));
    sps->subset = subset;
    sps->profile_idc = br_u(br, 8);
    sps->constraint_flags = br_u(br, 8);
    sps->level_idc = br_u(br, 8);
    uint32_t id = br_ue(br);
    if (id > 31)
        return "bad SPS id";
    sps->id = id;
    sps->chroma_format_idc = 1;
    sps->bit_depth_luma = sps->bit_depth_chroma = 8;
    sps->tools_start = br->pos;
    sps->high = high_profile(sps->profile_idc);
    if (sps->high) {
        uint32_t format = br_ue(br);
        if (format > 3)
            return "bad chroma_format_idc";
        sps->chroma_format_idc = format;
        if (sps->chroma_format_idc == 3)
            sps->separate_colour_plane = br_u1(br);
        uint32_t luma = br_ue(br), chroma = br_ue(br);
        if (luma > 6 || chroma > 6)
            return "bad bit depth";
        sps->bit_depth_luma = 8 + luma;
        sps->bit_depth_chroma = 8 + chroma;
        sps->lossless = br_u1(br);
        sps->scaling_matrix = br_u1(br);
        if (sps->scaling_matrix) {
            uint8_t tmp[64];
            for (int i = 0; i < (sps->chroma_format_idc != 3 ? 8 : 12); i++) {
                bool use_default;
                if (!br_u1(br)) {
                    if (i < 8)
                        fall_back(&sps->lists, i, NULL);
                    continue;
                }
                uint8_t *l = i < 8 ? list_at(&sps->lists, i) : tmp;
                if (!read_scaling_list(br, l, i < 6 ? 16 : 64, &use_default))
                    return "bad scaling list";
                if (use_default && i < 8)
                    memcpy(l, default_list(i), i < 6 ? 16 : 64);
            }
        }
    }
    if (!sps->scaling_matrix)
        flat(&sps->lists);
    sps->tools_end = br->pos;
    uint32_t v = br_ue(br);
    if (v > 12)
        return "bad log2_max_frame_num";
    sps->log2_max_frame_num = v + 4;
    v = br_ue(br);
    if (v > 2)
        return "bad pic_order_cnt_type";
    sps->poc_type = v;
    if (sps->poc_type == 0) {
        v = br_ue(br);
        if (v > 12)
            return "bad log2_max_pic_order_cnt_lsb";
        sps->log2_max_poc_lsb = v + 4;
    } else if (sps->poc_type == 1) {
        sps->delta_pic_order_always_zero = br_u1(br);
        sps->offset_for_non_ref_pic = br_se(br);
        sps->offset_for_top_to_bottom_field = br_se(br);
        v = br_ue(br);
        if (v > 255)
            return "bad num_ref_frames_in_pic_order_cnt_cycle";
        sps->num_ref_frames_in_poc_cycle = v;
        int64_t sum = 0;
        for (uint32_t i = 0; i < v; i++) {
            sps->offset_for_ref_frame[i] = br_se(br);
            sum += sps->offset_for_ref_frame[i];
        }
        // Kept to 32 bits, so the order counts computed from it fit 64.
        if (sum < INT32_MIN || sum > INT32_MAX)
            return "bad offset_for_ref_frame";
        sps->expected_delta_per_poc_cycle = (int32_t)sum;
    }
    v = br_ue(br);
    if (v > MAX_REF_FRAMES)
        return "bad max_num_ref_frames";
    sps->max_num_ref_frames = v;
    sps->gaps_allowed = br_u1(br);
    v = br_ue(br);
    uint32_t h = br_ue(br);
    if (v > 1023 || h > 1023)
        return "bad picture size";
    sps->width_mbs = v + 1;
    sps->frame_mbs_only = br_u1(br);
    sps->height_mbs = (h + 1) * (sps->frame_mbs_only ? 1 : 2);
    if (!sps->frame_mbs_only)
        sps->mbaff = br_u1(br);
    sps->direct_8x8_inference = br_u1(br);
    sps->cropping = br_u1(br);
    if (sps->cropping) {
        for (int i = 0; i < 4; i++)
            sps->crop[i] = br_ue(br);
    }
    sps->vui = br_u1(br);
    if (sps->vui) {
        sps->aspect_present = br_u1(br);
        if (sps->aspect_present) {
            sps->aspect_idc = br_u(br, 8);
            if (sps->aspect_idc == 255) {
                sps->sar_w = br_u(br, 16);
                sps->sar_h = br_u(br, 16);
            }
        }
        sps->overscan_present = br_u1(br);
        if (sps->overscan_present)
            sps->overscan_appropriate = br_u1(br);
        sps->signal_present = br_u1(br);
        if (sps->signal_present) {
            sps->video_format = br_u(br, 3);
            sps->full_range = br_u1(br);
            sps->colour_present = br_u1(br);
            if (sps->colour_present) {
                sps->primaries = br_u(br, 8);
                sps->transfer = br_u(br, 8);
                sps->matrix = br_u(br, 8);
            }
        }
        sps->chroma_loc_present = br_u1(br);
        if (sps->chroma_loc_present) {
            sps->chroma_loc_top = br_ue(br);
            sps->chroma_loc_bottom = br_ue(br);
        }
        if (br_u1(br)) {  // timing_info_present_flag
            br_u(br, 32);
            br_u(br, 32);
            br_u1(br);
        }
        bool nal_hrd = br_u1(br);
        if (nal_hrd && !skip_hrd(br))
            return "bad HRD parameters";
        bool vcl_hrd = br_u1(br);
        if (vcl_hrd && !skip_hrd(br))
            return "bad HRD parameters";
        if (nal_hrd || vcl_hrd)
            br_u1(br);  // low_delay_hrd_flag
        br_u1(br);      // pic_struct_present_flag
        sps->restriction = br_u1(br);
        if (sps->restriction) {
            sps->mv_over_boundaries = br_u1(br);
            sps->max_bytes_per_pic_denom = br_ue(br);
            sps->max_bits_per_mb_denom = br_ue(br);
            sps->log2_max_mv_h = br_ue(br);
            sps->log2_max_mv_v = br_ue(br);
            sps->num_reorder_frames = br_ue(br);
            sps->max_dec_frame_buffering = br_ue(br);
        }
    }
    if (br->overrun)
        return "truncated SPS";
    if (subset && (sps->profile_idc == 118 || sps->profile_idc == 128 ||
                   sps->profile_idc == 134)) {
        br_u1(br);  // bit_equal_to_one
        v = br_ue(br);
        if (v > 1023)
            return "bad num_views_minus1";
        sps->num_views = v + 1;
        for (int i = 0; i < sps->num_views; i++) {
            uint32_t view_id = br_ue(br);
            if (br->overrun)
                return "truncated subset SPS";
            if (view_id > 1023)
                return "bad view_id";
            if (i < MAX_VIEWS_KEPT)
                sps->view_id[i] = view_id;
        }
        for (int anchor = 1; anchor >= 0; anchor--) {
            for (int i = 1; i < sps->num_views; i++) {
                for (int list = 0; list < 2; list++) {
                    v = br_ue(br);
                    if (v > 15 || br->overrun)
                        return "bad inter-view references";
                    for (uint32_t j = 0; j < v; j++) {
                        uint32_t ref = br_ue(br);
                        if (ref > 1023)
                            return "bad inter-view references";
                        if (i == 1)
                            sps->refs[anchor][list][j] = ref;
                    }
                    if (i == 1)
                        sps->refs_count[anchor][list] = v;
                }
            }
        }
        if (br->overrun)
            return "truncated subset SPS";
    }
    sps->rbsp = malloc(size ? size : 1);
    if (!sps->rbsp)
        return "out of memory";
    memcpy(sps->rbsp, rbsp, size);
    sps->rbsp_size = size;
    sps->valid = true;
    return NULL;
}

const char *parse_pps(struct pps *pps, const uint8_t *rbsp, size_t size)
{
    struct bitreader b, *br = &b;
    br_init(br, rbsp, size);
    memset(pps, 0, sizeof(*pps));
    uint32_t id = br_ue(br), sps_id = br_ue(br);
    if (id > 255 || sps_id > 31)
        return "bad PPS id";
    pps->id = id;
    pps->sps_id = sps_id;
    pps->rest_start = br->pos;
    pps->cabac = br_u1(br);
    pps->bottom_field_pic_order = br_u1(br);
    uint32_t groups = br_ue(br);
    if (groups > 7)
        return "bad num_slice_groups";
    pps->num_slice_groups = groups + 1;
    if (pps->num_slice_groups == 1) {
        // Slice groups aren't parsed: the rewrite refuses them anyway.
        for (int i = 0; i < 2; i++) {
            uint32_t n = br_ue(br);
            if (n > 31)
                return "bad num_ref_idx_default_active";
            pps->num_ref_idx_default[i] = n + 1;
        }
        pps->weighted_pred = br_u1(br);
        pps->weighted_bipred_idc = br_u(br, 2);
        br_se(br);  // pic_init_qp_minus26
        br_se(br);  // pic_init_qs_minus26
        pps->chroma_qp_index_offset = br_se(br);
        pps->deblocking_control = br_u1(br);
        br_u1(br);  // constrained_intra_pred_flag
        pps->redundant_pic_cnt = br_u1(br);
    }
    if (br->overrun)
        return "truncated PPS";
    pps->stop_bit = find_stop_bit(rbsp, size);
    if (pps->stop_bit < br->pos)
        return "truncated PPS";
    pps->ext_start = br->pos;
    pps->ext = br->pos < pps->stop_bit;
    pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    pps->ext_ok = true;
    if (pps->ext && pps->num_slice_groups == 1) {
        // Read for 4:2:0 and 4:2:2; 4:4:4 adds lists the rewrite refuses.
        pps->transform_8x8 = br_u1(br);
        pps->pic_scaling_matrix = br_u1(br);
        if (pps->pic_scaling_matrix) {
            for (int i = 0; i < 6 + 2 * pps->transform_8x8; i++) {
                pps->list_present[i] = br_u1(br);
                if (pps->list_present[i] &&
                    !read_scaling_list(br, list_at(&pps->raw, i), i < 6 ? 16 : 64,
                                       &pps->list_default[i]))
                    pps->ext_ok = false;
            }
        }
        pps->second_chroma_qp_index_offset = br_se(br);
        if (br->overrun || br->pos != pps->stop_bit)
            pps->ext_ok = false;
    }
    pps->rbsp = malloc(size ? size : 1);
    if (!pps->rbsp)
        return "out of memory";
    memcpy(pps->rbsp, rbsp, size);
    pps->rbsp_size = size;
    pps->valid = true;
    return NULL;
}

const char *parse_slice_header(struct slice *s, const uint8_t *rbsp, size_t size,
                               int nal_type, int nal_ref_idc,
                               struct pps *const *pps_table,
                               struct sps *const *sps_table)
{
    struct bitreader b, *br = &b;
    br_init(br, rbsp, size);
    s->rbsp = rbsp;
    s->rbsp_size = size;
    s->nal_ref_idc = nal_ref_idc;
    uint32_t first_mb = br_ue(br), raw_type = br_ue(br);
    if (raw_type > 9)
        return "bad slice_type";
    s->raw_type = raw_type;
    s->type = s->raw_type % 5;
    uint32_t pps_id = br_ue(br);
    if (pps_id > 255 || !pps_table[pps_id] || !pps_table[pps_id]->valid)
        return "slice refers to a missing PPS";
    s->pps_id = pps_id;
    const struct pps *pps = s->pps = pps_table[pps_id];
    const struct sps *sps = sps_table[pps->sps_id];
    if (!sps || !sps->valid)
        return "slice refers to a missing SPS";
    s->sps = sps;
    if (first_mb >= (uint32_t)(sps->width_mbs * sps->height_mbs))
        return "bad first_mb_in_slice";
    s->first_mb = first_mb;
    if (pps->num_slice_groups > 1)
        return NULL;  // refused by the caller, nothing more to read
    if (sps->separate_colour_plane)
        br_u(br, 2);
    s->frame_num = br_u(br, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only && br_u1(br))
        return "field picture";
    if (s->idr) {
        uint32_t id = br_ue(br);
        if (id > 65535)
            return "bad idr_pic_id";
        s->idr_pic_id = id;
        (void)nal_type;
    }
    s->delta_poc_bottom = s->delta_poc[0] = s->delta_poc[1] = 0;
    if (sps->poc_type == 0) {
        s->poc_lsb = br_u(br, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order)
            s->delta_poc_bottom = br_se(br);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        s->delta_poc[0] = br_se(br);
        if (pps->bottom_field_pic_order)
            s->delta_poc[1] = br_se(br);
    }
    if (pps->redundant_pic_cnt)
        br_ue(br);
    if (s->type == SLICE_B)
        s->direct_spatial = br_u1(br);
    s->num_ref_idx[0] = s->num_ref_idx[1] = 0;
    if (s->type == SLICE_P || s->type == SLICE_SP || s->type == SLICE_B) {
        s->num_ref_idx[0] = pps->num_ref_idx_default[0];
        if (s->type == SLICE_B)
            s->num_ref_idx[1] = pps->num_ref_idx_default[1];
        if (br_u1(br)) {
            // Up to 16 for frames: checked before they become counts.
            uint32_t n0 = br_ue(br), n1 = s->type == SLICE_B ? br_ue(br) : 0;
            if (n0 > 15 || n1 > 15)
                return "bad num_ref_idx_active";
            s->num_ref_idx[0] = n0 + 1;
            if (s->type == SLICE_B)
                s->num_ref_idx[1] = n1 + 1;
        }
        if (s->num_ref_idx[0] > 16 || s->num_ref_idx[1] > 16)
            return "bad num_ref_idx_active";
    }
    int lists = s->type == SLICE_B ? 2 : s->num_ref_idx[0] ? 1 : 0;
    s->nmods[0] = s->nmods[1] = 0;
    for (int l = 0; l < lists; l++) {
        if (!br_u1(br))
            continue;
        for (;;) {
            uint32_t idc = br_ue(br);
            if (idc == 3 || br->overrun)
                break;
            if (idc > 5 || (idc > 2 && nal_type != 20))
                return "bad modification_of_pic_nums_idc";
            if (s->nmods[l] == MAX_MODS)
                return "too many reference list modifications";
            s->mods[l][s->nmods[l]].idc = idc;
            s->mods[l][s->nmods[l]].val = br_ue(br);
            s->nmods[l]++;
        }
    }
    s->wpt_start = s->wpt_end = br->pos;
    if ((pps->weighted_pred && (s->type == SLICE_P || s->type == SLICE_SP)) ||
        (pps->weighted_bipred_idc == 1 && s->type == SLICE_B)) {
        bool chroma = sps->chroma_format_idc && !sps->separate_colour_plane;
        br_ue(br);
        if (chroma)
            br_ue(br);
        for (int l = 0; l < lists; l++) {
            for (int i = 0; i < s->num_ref_idx[l]; i++) {
                if (br_u1(br)) {
                    br_se(br);
                    br_se(br);
                }
                if (chroma && br_u1(br)) {
                    for (int k = 0; k < 4; k++)
                        br_se(br);
                }
            }
        }
        s->wpt_end = br->pos;
    }
    s->no_output_of_prior_pics = s->long_term_reference = s->adaptive = false;
    s->nmmco = 0;
    if (nal_ref_idc) {
        if (s->idr) {
            s->no_output_of_prior_pics = br_u1(br);
            s->long_term_reference = br_u1(br);
        } else {
            s->adaptive = br_u1(br);
            while (s->adaptive) {
                uint32_t op = br_ue(br);
                if (!op || br->overrun)
                    break;
                if (op > 6)
                    return "bad memory_management_control_operation";
                if (s->nmmco == MAX_MMCO)
                    return "too many memory management operations";
                struct mmco *m = &s->mmco[s->nmmco++];
                m->op = op;
                m->a = m->b = 0;
                if (op != 5)
                    m->a = br_ue(br);
                if (op == 3)
                    m->b = br_ue(br);
            }
        }
    }
    s->tail_start = br->pos;
    if (pps->cabac && s->type != SLICE_I && s->type != SLICE_SI)
        br_ue(br);  // cabac_init_idc
    br_se(br);      // slice_qp_delta
    if (s->type == SLICE_SP || s->type == SLICE_SI) {
        if (s->type == SLICE_SP)
            br_u1(br);
        br_se(br);
    }
    if (pps->deblocking_control && br_ue(br) != 1) {
        br_se(br);
        br_se(br);
    }
    s->header_end = br->pos;
    s->stop_bit = find_stop_bit(rbsp, size);
    if (br->overrun || s->header_end > s->stop_bit)
        return "truncated slice header";
    return NULL;
}
