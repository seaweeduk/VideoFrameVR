/* MVC to frame-sequential H.264, see mvc_rewrite.h.
 *
 * Each view is followed as an MVC decoder sees it: its own picture order
 * count (G.8.1), its reference lists (G.8.2: its own pictures, plus the base
 * picture of the access unit for the dependent view) and its own reference
 * marking (G.8.3). The rewritten stream holds both views' reference
 * pictures in one DPB. Every picture's lists are written out in full with
 * modification commands, so the plain decoder builds exactly the lists the
 * MVC decoder builds, and every reference picture's marking is written as
 * explicit commands (MMCO 1) that drop what its view's marking drops.
 *
 * The base picture of an access unit that the dependent view predicts from
 * is made a reference even when it isn't one in its view, and dropped after
 * the dependent picture. An MVC decoder treats such an inter-view reference
 * as neither short- nor long-term (G.8.4), which matters only for spatial
 * direct prediction from RefPicList1[0] (colZeroFlag, 8.4.1.2.2). When a
 * dependent B slice has the base picture there and the base picture isn't
 * all intra, the dependent view predicts instead from a hidden copy of it,
 * a picture of P_Skip macroblocks marked long-term, for which colZeroFlag is
 * 0 as well.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "mvc_rewrite.h"
#include "mvc_internal.h"

#define POOL_SIZE 48
#define MAX_MERGED_REFS 16
#define MAX_REMOVALS 40
#define MERGED_LOG2_MAX_FRAME_NUM 16
#define MERGED_MAX_FRAME_NUM (1 << MERGED_LOG2_MAX_FRAME_NUM)
#define MERGED_LOG2_MAX_POC_LSB 16

struct pic {
    bool used;
    int view;         // 0 base, 1 dependent, 2 hidden copy
    int frame_num;    // in its own view
    int poc;          // its view's order count
    int mfn;          // frame_num in the rewritten stream
    bool view_ref;    // "used for short-term reference" in its own view
    bool merged_ref;  // a reference in the rewritten stream
    bool long_term;   // long-term in the rewritten stream (hidden copies)
};

struct view_state {
    struct pic *refs[MAX_REF_FRAMES + 1];
    int nrefs;
    int max_frame_num;
    int prev_ref_frame_num;
    int prev_poc_msb, prev_poc_lsb;
    int prev_frame_num_offset, prev_frame_num;
};

struct picture_in {
    struct slice *slices;
    int count, cap;
};

struct pps_map {
    bool used;
    int view, src_id;
    bool bottom;        // bottom_field_pic_order_in_frame_present_flag written
    unsigned gen;       // source PPS generation it was written from
    bool emitted;       // since the last IDR or reset
    // CAVLC: copies without the bottom field order count, with ids coded in
    // 9, 11, 13 and 15 bits (see write_slice).
    int variants[4];
};

// Picture order counts in the rewritten stream, relative to the IDR's P:
// EXACT keeps the source's distances (base P, dependent P + 1), which
// temporal direct and implicit weighted prediction need; it requires even
// source counts and no hidden copies. SPREAD (base 4P, hidden copy 4P + 1,
// dependent 4P + 2) has room for both. Which one an IDR period needs shows
// only in its pictures, so its first access units are held (see
// mvc_rewrite) until they tell, and rewritten again if the guess was wrong.
enum poc_mode { POC_EXACT, POC_SPREAD };

#define HOLD_MAX 16
#define MAX_PICTURES_PER_AU 3
// rewrite_au's answer when the access unit needs the other layout
#define LAYOUT_MISMATCH ((enum mvc_status)100)

struct out_pic {
    enum mvc_slot slot;
    size_t start, size;
    bool idr;
    int32_t poc;
    int64_t tag;
};

struct held_au {
    size_t start, size;
    int64_t tag;
};

struct mvc_rewriter {
    int length_size;  // 0: Annex B

    struct sps *sps[32], *ssps[32];
    struct pps *pps[256];
    unsigned sps_gen[32], ssps_gen[32], pps_gen[256];

    bool unsupported, waiting;
    const char *reason;
    enum poc_mode poc_mode;
    bool holding;                       // the period's layout isn't settled
    bool period_exact, period_spread;   // what the period's pictures need
    bool seen_exact;                    // any period needed EXACT
    bool au_plain_b;  // B slices that don't depend on order count distances
    uint8_t *hold;                      // the period's access units, as given
    size_t hold_size, hold_cap;
    struct held_au *held;
    int nheld, held_cap;

    // Active parameter sets, by value, and the generations they came from.
    struct sps base_sps, dep_sps;
    unsigned base_sps_gen, dep_sps_gen;
    int base_view_id;

    // Rewritten SPS settings
    int merged_profile, merged_level, merged_refs_max;
    int merged_reorder, merged_dpb;

    struct view_state views[2];
    struct pic pool[POOL_SIZE];
    int prev_ref_mfn;
    int32_t idr_poc;
    int32_t prev_ref_out_poc;
    bool lt_enabled;
    struct pic *pending[MAX_REMOVALS];
    int npending;

    struct pps_map map[256];
    int map_of[2][256];  // [view][source pps id] -> rewritten id, or -1
    int standin_pps;     // rewritten id of the hidden copies' PPS, or -1
    bool standin_pps_emitted;

    // Per access unit
    uint8_t *rbsp;
    size_t rbsp_cap, rbsp_used;
    struct picture_in in[2];
    struct bitwriter bw;
    uint8_t *out;
    size_t out_cap, out_size;
    // Pictures written and not yet returned; the first nreleased were
    // returned by the last call, the period's start at period_pic.
    struct out_pic *pics;
    int npics, pics_cap, nreleased, period_pic;
    struct mvc_picture *pub;
    int pub_cap;
    int64_t tag;
};

static const char *const no_memory = "out of memory";

static void free_tables(struct mvc_rewriter *r)
{
    for (int i = 0; i < 32; i++) {
        if (r->sps[i])
            free(r->sps[i]->rbsp);
        if (r->ssps[i])
            free(r->ssps[i]->rbsp);
        free(r->sps[i]);
        free(r->ssps[i]);
    }
    for (int i = 0; i < 256; i++) {
        if (r->pps[i])
            free(r->pps[i]->rbsp);
        free(r->pps[i]);
    }
}

void mvc_rewriter_destroy(struct mvc_rewriter *r)
{
    if (!r)
        return;
    free_tables(r);
    free(r->rbsp);
    free(r->in[0].slices);
    free(r->in[1].slices);
    free(r->bw.data);
    free(r->out);
    free(r->hold);
    free(r->held);
    free(r->pics);
    free(r->pub);
    free(r->base_sps.rbsp);
    free(r);
}

static void clear_pictures(struct mvc_rewriter *r)
{
    memset(r->pool, 0, sizeof(r->pool));
    memset(r->views, 0, sizeof(r->views));
    r->npending = 0;
    r->lt_enabled = false;
    r->prev_ref_mfn = 0;
}

void mvc_rewriter_reset(struct mvc_rewriter *r)
{
    clear_pictures(r);
    r->holding = false;
    r->nheld = 0;
    r->hold_size = 0;
    r->npics = r->nreleased = r->period_pic = 0;
    r->out_size = 0;
    r->waiting = true;
    for (int i = 0; i < 256; i++)
        r->map[i].emitted = false;
    r->standin_pps_emitted = false;
}

const char *mvc_rewriter_reason(const struct mvc_rewriter *r)
{
    return r->reason ? r->reason : "";
}

/* NAL units */

// Removes emulation prevention bytes; returns the RBSP size.
static size_t unescape(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t o = 0, i = 0;
    while (i < n) {
        const uint8_t *z = memchr(src + i, 0, n - i);
        size_t run = z ? (size_t)(z - (src + i)) : n - i;
        memcpy(dst + o, src + i, run);
        o += run;
        i += run;
        if (i >= n)
            break;
        // src[i] == 0
        if (i + 2 < n && src[i + 1] == 0 && src[i + 2] == 3) {
            dst[o++] = 0;
            dst[o++] = 0;
            i += 3;
        } else {
            dst[o++] = 0;
            i++;
        }
    }
    return o;
}

static bool out_reserve(struct mvc_rewriter *r, size_t more)
{
    if (r->out_size + more <= r->out_cap)
        return true;
    size_t cap = r->out_cap ? r->out_cap : 65536;
    while (cap < r->out_size + more)
        cap *= 2;
    uint8_t *p = realloc(r->out, cap);
    if (!p)
        return false;
    r->out = p;
    r->out_cap = cap;
    return true;
}

// Appends start code, NAL header and the escaped RBSP held in r->bw.
static bool emit_nal(struct mvc_rewriter *r, int nal_ref_idc, int type)
{
    struct bitwriter *bw = &r->bw;
    if (bw->failed)
        return false;
    size_t n = bw->pos / 8;
    // Worst case one 0x03 per two input bytes.
    if (!out_reserve(r, 5 + n + n / 2 + 2))
        return false;
    uint8_t *o = r->out + r->out_size;
    *o++ = 0;
    *o++ = 0;
    *o++ = 0;
    *o++ = 1;
    *o++ = (uint8_t)((nal_ref_idc << 5) | type);
    const uint8_t *s = bw->data;
    size_t i = 0;
    int zeros = 0;
    while (i < n) {
        if (zeros < 2) {
            // Copy up to the next zero byte in one go.
            const uint8_t *z = memchr(s + i, 0, n - i);
            size_t run = z ? (size_t)(z - (s + i)) : n - i;
            if (run) {
                memcpy(o, s + i, run);
                o += run;
                i += run;
                zeros = 0;
                continue;
            }
        }
        uint8_t b = s[i++];
        if (zeros >= 2 && b <= 3) {
            *o++ = 3;
            zeros = 0;
        }
        *o++ = b;
        zeros = b ? 0 : zeros + 1;
    }
    if (n && !s[n - 1])
        *o++ = 3;
    r->out_size = o - r->out;
    return true;
}

static void bw_start(struct mvc_rewriter *r)
{
    r->bw.pos = 0;
    r->bw.failed = false;
}

/* Parameter sets */

static const char *store_sps(struct mvc_rewriter *r, const uint8_t *rbsp, size_t size,
                             bool subset)
{
    struct sps tmp;
    const char *err = parse_sps(&tmp, rbsp, size, subset);
    if (err)
        return err;
    struct sps **slot = subset ? &r->ssps[tmp.id] : &r->sps[tmp.id];
    unsigned *gen = subset ? &r->ssps_gen[tmp.id] : &r->sps_gen[tmp.id];
    if (*slot && (*slot)->rbsp_size == tmp.rbsp_size &&
        !memcmp((*slot)->rbsp, tmp.rbsp, tmp.rbsp_size)) {
        free(tmp.rbsp);
        return NULL;
    }
    if (!*slot && !(*slot = calloc(1, sizeof(**slot)))) {
        free(tmp.rbsp);
        return no_memory;
    }
    free((*slot)->rbsp);
    **slot = tmp;
    (*gen)++;
    return NULL;
}

static const char *store_pps(struct mvc_rewriter *r, const uint8_t *rbsp, size_t size)
{
    struct pps tmp;
    const char *err = parse_pps(&tmp, rbsp, size);
    if (err)
        return err;
    struct pps **slot = &r->pps[tmp.id];
    if (*slot && (*slot)->rbsp_size == tmp.rbsp_size &&
        !memcmp((*slot)->rbsp, tmp.rbsp, tmp.rbsp_size)) {
        free(tmp.rbsp);
        return NULL;
    }
    if (!*slot && !(*slot = calloc(1, sizeof(**slot)))) {
        free(tmp.rbsp);
        return no_memory;
    }
    free((*slot)->rbsp);
    **slot = tmp;
    r->pps_gen[tmp.id]++;
    return NULL;
}

// Table A-1: level_idc, MaxMBPS, MaxFS, MaxDpbMbs.
static const struct {
    int idc;
    int mbps, fs, dpb;
} levels[] = {
    {10, 1485, 99, 396},          {11, 3000, 396, 900},
    {12, 6000, 396, 2376},        {13, 11880, 396, 2376},
    {20, 11880, 396, 2376},       {21, 19800, 792, 4752},
    {22, 20250, 1620, 8100},      {30, 40500, 1620, 8100},
    {31, 108000, 3600, 18000},    {32, 216000, 5120, 20480},
    {40, 245760, 8192, 32768},    {41, 245760, 8192, 32768},
    {42, 522240, 8704, 34816},    {50, 589824, 22080, 110400},
    {51, 983040, 36864, 184320},  {52, 2073600, 36864, 184320},
    {60, 4177920, 139264, 696320}, {61, 8355840, 139264, 696320},
    {62, 16711680, 139264, 696320},
};
#define NUM_LEVELS ((int)(sizeof(levels) / sizeof(levels[0])))

static int level_index(int idc)
{
    for (int i = 0; i < NUM_LEVELS; i++) {
        if (levels[i].idc >= idc)
            return i;
    }
    return NUM_LEVELS - 1;
}

static uint32_t max_dpb_frames(const struct sps *sps)
{
    int mbs = sps->width_mbs * sps->height_mbs;
    int n = levels[level_index(sps->level_idc)].dpb / mbs;
    return n < 1 ? 1 : n > 16 ? 16 : (uint32_t)n;
}

static bool same_tools(const struct sps *a, const struct sps *b)
{
    if (a->chroma_format_idc != b->chroma_format_idc ||
        a->separate_colour_plane != b->separate_colour_plane ||
        a->bit_depth_luma != b->bit_depth_luma ||
        a->bit_depth_chroma != b->bit_depth_chroma || a->lossless != b->lossless)
        return false;
    // Different scaling lists go into the dependent view's PPSs, which can't
    // hold 4:4:4's extra lists.
    return a->chroma_format_idc != 3 || !memcmp(&a->lists, &b->lists, sizeof(a->lists));
}

// Checks that both views can share one SPS and works out its settings.
static const char *activate(struct mvc_rewriter *r, const struct sps *base,
                            const struct sps *dep)
{
    if (dep->profile_idc != 118 && dep->profile_idc != 128)
        return "not a Multiview High or Stereo High stream";
    if (dep->num_views < 2)
        return "fewer than two views";
    for (int a = 0; a < 2; a++) {
        for (int l = 0; l < 2; l++) {
            for (int j = 0; j < dep->refs_count[a][l]; j++) {
                if (dep->refs[a][l][j] != dep->view_id[0])
                    return "second view predicts from a third one";
            }
        }
    }
    if (!base->frame_mbs_only || !dep->frame_mbs_only)
        return "field or MBAFF coding";
    if (base->separate_colour_plane)
        return "separate colour planes";
    if (!same_tools(base, dep))
        return "views differ in chroma format or bit depth";
    if (base->width_mbs != dep->width_mbs || base->height_mbs != dep->height_mbs ||
        base->direct_8x8_inference != dep->direct_8x8_inference ||
        base->cropping != dep->cropping ||
        memcmp(base->crop, dep->crop, sizeof(base->crop)))
        return "views differ in size or cropping";

    int p = base->profile_idc;
    r->merged_profile = (p == 100 || p == 110 || p == 122 || p == 244) ? p : 100;
    int refs = base->max_num_ref_frames + dep->max_num_ref_frames + 2;
    r->merged_refs_max = refs > MAX_MERGED_REFS ? MAX_MERGED_REFS : refs;
    // The rewritten stream holds and reorders both views' pictures. A base
    // view that says it reorders more frames than the rewritten stream can
    // (16 pictures) is refused: its pictures would come out of the decoder
    // in the wrong order. Without the VUI's figures, the level's DPB size
    // stands in, an upper bound (the most a small picture's level allows),
    // and the stream gets as much as it can hold. The DPB sizes are upper
    // bounds too (declared generously: 12 per view in a conformance stream
    // that reorders 4); the reference pictures actually held are checked
    // per picture against merged_refs_max.
    uint32_t reorder = base->restriction ? base->num_reorder_frames : max_dpb_frames(base);
    uint32_t dpb_base = base->restriction ? base->max_dec_frame_buffering : max_dpb_frames(base);
    uint32_t dpb_dep = dep->restriction ? dep->max_dec_frame_buffering : max_dpb_frames(dep);
    if (reorder > 16 || dpb_base > 16 || dpb_dep > 16)
        return "bad reorder depth or DPB size";
    int dpb = (int)(dpb_base + dpb_dep) + 3;
    if (dpb < r->merged_refs_max)
        dpb = r->merged_refs_max;
    // Each access unit is two or three pictures in display order.
    int merged_reorder = (int)reorder * (r->poc_mode == POC_SPREAD ? 3 : 2);
    if (dpb < merged_reorder)
        dpb = merged_reorder;
    if (merged_reorder > 16 && base->restriction)
        return "views reorder more pictures than one stream can";
    r->merged_dpb = dpb > 16 ? 16 : dpb;
    r->merged_reorder = merged_reorder > 16 ? 16 : merged_reorder;

    // Lowest level that holds the DPB at twice the base view's rate.
    int mbs = base->width_mbs * base->height_mbs;
    int li = level_index(base->level_idc);
    int mbps = levels[li].mbps * 2;
    while (li < NUM_LEVELS - 1 &&
           (levels[li].dpb < r->merged_dpb * mbs || levels[li].fs < mbs ||
            levels[li].mbps < mbps))
        li++;
    r->merged_level = levels[li].idc;
    return NULL;
}

static void write_sps(struct mvc_rewriter *r)
{
    const struct sps *s = &r->base_sps;
    struct bitwriter *bw = &r->bw;
    bw_start(r);
    bw_u(bw, 8, r->merged_profile);
    bw_u(bw, 8, 0);
    bw_u(bw, 8, r->merged_level);
    bw_ue(bw, 0);
    if (s->high) {
        bw_copy(bw, s->rbsp, s->tools_start, s->tools_end - s->tools_start);
    } else {
        bw_ue(bw, 1);
        bw_ue(bw, 0);
        bw_ue(bw, 0);
        bw_u(bw, 1, 0);
        bw_u(bw, 1, 0);
    }
    bw_ue(bw, MERGED_LOG2_MAX_FRAME_NUM - 4);
    bw_ue(bw, 0);
    bw_ue(bw, MERGED_LOG2_MAX_POC_LSB - 4);
    bw_ue(bw, r->merged_refs_max);
    bw_u(bw, 1, 0);
    bw_ue(bw, s->width_mbs - 1);
    bw_ue(bw, s->height_mbs - 1);
    bw_u(bw, 1, 1);
    bw_u(bw, 1, s->direct_8x8_inference);
    bw_u(bw, 1, s->cropping);
    if (s->cropping) {
        for (int i = 0; i < 4; i++)
            bw_ue(bw, s->crop[i]);
    }
    // VUI: what describes the picture is kept. Timing and HRD would
    // describe the source stream, so they go; the reorder depth and DPB
    // size are this stream's.
    bw_u(bw, 1, 1);
    bw_u(bw, 1, s->aspect_present);
    if (s->aspect_present) {
        bw_u(bw, 8, s->aspect_idc);
        if (s->aspect_idc == 255) {
            bw_u(bw, 16, s->sar_w);
            bw_u(bw, 16, s->sar_h);
        }
    }
    bw_u(bw, 1, s->overscan_present);
    if (s->overscan_present)
        bw_u(bw, 1, s->overscan_appropriate);
    bw_u(bw, 1, s->signal_present);
    if (s->signal_present) {
        bw_u(bw, 3, s->video_format);
        bw_u(bw, 1, s->full_range);
        bw_u(bw, 1, s->colour_present);
        if (s->colour_present) {
            bw_u(bw, 8, s->primaries);
            bw_u(bw, 8, s->transfer);
            bw_u(bw, 8, s->matrix);
        }
    }
    bw_u(bw, 1, s->chroma_loc_present);
    if (s->chroma_loc_present) {
        bw_ue(bw, s->chroma_loc_top);
        bw_ue(bw, s->chroma_loc_bottom);
    }
    bw_u(bw, 1, 0);  // timing_info_present_flag
    bw_u(bw, 1, 0);  // nal_hrd_parameters_present_flag
    bw_u(bw, 1, 0);  // vcl_hrd_parameters_present_flag
    bw_u(bw, 1, 0);  // pic_struct_present_flag
    bw_u(bw, 1, 1);  // bitstream_restriction_flag
    bw_u(bw, 1, s->restriction ? s->mv_over_boundaries : 1);
    bw_ue(bw, s->restriction ? s->max_bytes_per_pic_denom : 0);
    bw_ue(bw, s->restriction ? s->max_bits_per_mb_denom : 0);
    bw_ue(bw, s->restriction ? s->log2_max_mv_h : 15);
    bw_ue(bw, s->restriction ? s->log2_max_mv_v : 15);
    bw_ue(bw, r->merged_reorder);
    bw_ue(bw, r->merged_dpb);
    bw_trailing(bw);
}

static int ue_bits(uint32_t v)
{
    int n = 0;
    while (((uint64_t)v + 1) >> (n + 1))
        n++;
    return 2 * n + 1;
}

static int free_pps_id(struct mvc_rewriter *r, int from, int to)
{
    for (int i = from; i <= to; i++) {
        if (!r->map[i].used && i != r->standin_pps)
            return i;
    }
    return -1;
}

// Rewritten PPS id for a view's PPS, assigned on first use.
static int map_pps(struct mvc_rewriter *r, int view, int src)
{
    int id = r->map_of[view][src];
    if (id < 0) {
        id = free_pps_id(r, 0, 254);
        if (id < 0)
            return -1;
        r->map[id] = (struct pps_map){.used = true, .view = view, .src_id = src,
                                      .variants = {-1, -1, -1, -1}};
        r->map_of[view][src] = id;
    }
    struct pps_map *m = &r->map[id];
    const struct pps *p = r->pps[src];
    m->bottom = p->cabac ? p->bottom_field_pic_order : true;
    if (!p->cabac && m->variants[0] < 0) {
        static const int ranges[4][2] = {{15, 30}, {31, 62}, {63, 126}, {127, 254}};
        for (int k = 0; k < 4; k++) {
            int v = free_pps_id(r, ranges[k][0], ranges[k][1]);
            if (v < 0)
                return -1;
            r->map[v] = (struct pps_map){.used = true, .view = view, .src_id = src,
                                         .variants = {-1, -1, -1, -1}};
            m->variants[k] = v;
        }
    }
    return id;
}

static bool emit_pps(struct mvc_rewriter *r, int id, bool bottom,
                     const struct scaling *lists)
{
    struct pps_map *m = &r->map[id];
    const struct pps *p = r->pps[m->src_id];
    struct bitwriter *bw = &r->bw;
    bw_start(r);
    bw_ue(bw, id);
    bw_ue(bw, 0);
    bw_u(bw, 1, p->cabac);
    bw_u(bw, 1, bottom);
    bw_copy(bw, p->rbsp, p->rest_start + 2, p->ext_start - p->rest_start - 2);
    if (lists) {
        bw_u(bw, 1, p->transform_8x8);
        bw_u(bw, 1, 1);
        write_scaling(bw, lists, p->transform_8x8);
        bw_se(bw, p->second_chroma_qp_index_offset);
    } else {
        bw_copy(bw, p->rbsp, p->ext_start, p->stop_bit - p->ext_start);
    }
    bw_trailing(bw);
    m->emitted = true;
    m->gen = r->pps_gen[m->src_id];
    return emit_nal(r, 3, 8);
}

// Writes the PPSs a picture's slices use that the decoder doesn't have yet.
// A dependent view whose scaling lists differ from the base view's gets them
// written out in its PPSs, as the shared SPS has the base view's.
static const char *write_needed_pps(struct mvc_rewriter *r, const struct picture_in *in,
                                    int view)
{
    for (int i = 0; i < in->count; i++) {
        int id = r->map_of[view][in->slices[i].pps_id];
        struct pps_map *m = &r->map[id];
        if (m->emitted && m->gen == r->pps_gen[m->src_id])
            continue;
        const struct pps *p = r->pps[m->src_id];
        struct scaling want, got, *lists = NULL;
        if (view) {
            pps_scaling(p, &r->dep_sps, &want);
            pps_scaling(p, &r->base_sps, &got);
            if (memcmp(&want, &got, sizeof(want))) {
                if (!p->ext_ok)
                    return "unreadable PPS scaling lists";
                lists = &want;
            }
        }
        if (!emit_pps(r, id, m->bottom, lists))
            return no_memory;
        for (int k = 0; k < 4 && m->variants[k] >= 0; k++) {
            if (!emit_pps(r, m->variants[k], false, lists))
                return no_memory;
        }
    }
    return NULL;
}

// The hidden copies' PPS: CAVLC, one reference, no weighting, deblocking
// control so their slices can turn the filter off.
static bool emit_standin_pps(struct mvc_rewriter *r)
{
    struct bitwriter *bw = &r->bw;
    bw_start(r);
    bw_ue(bw, r->standin_pps);
    bw_ue(bw, 0);
    bw_u(bw, 1, 0);  // entropy_coding_mode_flag
    bw_u(bw, 1, 0);  // bottom_field_pic_order_in_frame_present_flag
    bw_ue(bw, 0);    // num_slice_groups_minus1
    bw_ue(bw, 0);
    bw_ue(bw, 0);
    bw_u(bw, 1, 0);  // weighted_pred_flag
    bw_u(bw, 2, 0);  // weighted_bipred_idc
    bw_se(bw, 0);
    bw_se(bw, 0);
    bw_se(bw, 0);
    bw_u(bw, 1, 1);  // deblocking_filter_control_present_flag
    bw_u(bw, 1, 0);
    bw_u(bw, 1, 0);
    bw_trailing(bw);
    r->standin_pps_emitted = true;
    return emit_nal(r, 3, 8);
}

/* Order counts, lists and marking per view (G.8.1 to G.8.3) */

static int pic_num(const struct pic *p, int curr_frame_num, int max_frame_num)
{
    return p->frame_num > curr_frame_num ? p->frame_num - max_frame_num : p->frame_num;
}

// 8.2.1, for frames. Returns false when the order count overflows.
static bool compute_poc(struct view_state *v, const struct sps *sps,
                        const struct slice *s, int32_t *out)
{
    int64_t poc;
    if (sps->poc_type == 0) {
        int max_lsb = 1 << sps->log2_max_poc_lsb;
        int prev_msb = s->idr ? 0 : v->prev_poc_msb;
        int prev_lsb = s->idr ? 0 : v->prev_poc_lsb;
        int msb;
        if (s->poc_lsb < prev_lsb && prev_lsb - s->poc_lsb >= max_lsb / 2)
            msb = prev_msb + max_lsb;
        else if (s->poc_lsb > prev_lsb && s->poc_lsb - prev_lsb > max_lsb / 2)
            msb = prev_msb - max_lsb;
        else
            msb = prev_msb;
        int64_t top = (int64_t)msb + s->poc_lsb;
        int64_t bottom = top + s->delta_poc_bottom;
        poc = top < bottom ? top : bottom;
        if (s->nal_ref_idc) {
            v->prev_poc_msb = msb;
            v->prev_poc_lsb = s->poc_lsb;
        }
    } else {
        int64_t offset;
        if (s->idr)
            offset = 0;
        else if (v->prev_frame_num > s->frame_num)
            offset = (int64_t)v->prev_frame_num_offset + v->max_frame_num;
        else
            offset = v->prev_frame_num_offset;
        if (sps->poc_type == 1) {
            int64_t abs_frame_num = sps->num_ref_frames_in_poc_cycle ? offset + s->frame_num : 0;
            if (!s->nal_ref_idc && abs_frame_num > 0)
                abs_frame_num--;
            int64_t expected = 0;
            if (abs_frame_num > 0) {
                int64_t cycle = (abs_frame_num - 1) / sps->num_ref_frames_in_poc_cycle;
                int in_cycle = (abs_frame_num - 1) % sps->num_ref_frames_in_poc_cycle;
                expected = cycle * sps->expected_delta_per_poc_cycle;
                for (int i = 0; i <= in_cycle; i++)
                    expected += sps->offset_for_ref_frame[i];
            }
            if (!s->nal_ref_idc)
                expected += sps->offset_for_non_ref_pic;
            int64_t top = expected + s->delta_poc[0];
            int64_t bottom = top + sps->offset_for_top_to_bottom_field + s->delta_poc[1];
            poc = top < bottom ? top : bottom;
        } else {
            poc = s->idr ? 0 : 2 * (offset + s->frame_num) - (s->nal_ref_idc ? 0 : 1);
        }
        if (offset > INT32_MAX)
            return false;
        v->prev_frame_num_offset = (int)offset;
        v->prev_frame_num = s->frame_num;
    }
    // Small enough for the rewritten counts (up to 4 times the distance
    // from the IDR, plus 2) and their differences.
    if (poc < INT32_MIN / 16 || poc > INT32_MAX / 16)
        return false;
    *out = (int32_t)poc;
    return true;
}

static int cmp_poc_desc(const void *a, const void *b)
{
    int pa = (*(struct pic *const *)a)->poc, pb = (*(struct pic *const *)b)->poc;
    return (pa < pb) - (pa > pb);
}

static int cmp_poc_asc(const void *a, const void *b)
{
    return -cmp_poc_desc(a, b);
}

struct list_ctx {
    struct view_state *v;
    int view_id;
    int frame_num;
    int32_t poc;
    const struct sps *dep_sps;  // NULL for the base view
    struct pic *interview;      // the base picture, when it may be referenced
    int base_view_id;
};

// Places pic at *idx and drops its later copies (G-4, G-5, G-10).
static void list_insert(struct pic **list, int n, int *idx, struct pic *pic)
{
    for (int c = n; c > *idx; c--)
        list[c] = list[c - 1];
    list[(*idx)++] = pic;
    int nidx = *idx;
    for (int c = *idx; c <= n; c++) {
        if (list[c] != pic)
            list[nidx++] = list[c];
    }
}

static const char *build_lists(struct list_ctx *c, struct slice *s)
{
    struct view_state *v = c->v;
    int lists = s->type == SLICE_B ? 2 : (s->type == SLICE_P ? 1 : 0);
    struct pic *init[2][MAX_REF_FRAMES + 1];
    int count = v->nrefs;
    if (!lists)
        return NULL;
    if (s->type == SLICE_P) {
        // 8.2.4.2.1: descending PicNum
        for (int i = 0; i < count; i++)
            init[0][i] = v->refs[i];
        for (int i = 1; i < count; i++) {
            struct pic *p = init[0][i];
            int pn = pic_num(p, c->frame_num, v->max_frame_num), j = i;
            while (j > 0 && pic_num(init[0][j - 1], c->frame_num, v->max_frame_num) < pn) {
                init[0][j] = init[0][j - 1];
                j--;
            }
            init[0][j] = p;
        }
    } else {
        // 8.2.4.2.3
        struct pic *before[MAX_REF_FRAMES + 1], *after[MAX_REF_FRAMES + 1];
        int nb = 0, na = 0;
        for (int i = 0; i < count; i++) {
            if (v->refs[i]->poc < c->poc)
                before[nb++] = v->refs[i];
            else
                after[na++] = v->refs[i];
        }
        qsort(before, nb, sizeof(before[0]), cmp_poc_desc);
        qsort(after, na, sizeof(after[0]), cmp_poc_asc);
        for (int i = 0; i < nb; i++) {
            init[0][i] = before[i];
            init[1][na + i] = before[i];
        }
        for (int i = 0; i < na; i++) {
            init[0][nb + i] = after[i];
            init[1][i] = after[i];
        }
        if (count > 1 && !memcmp(init[0], init[1], count * sizeof(init[0][0]))) {
            init[1][0] = init[0][1];
            init[1][1] = init[0][0];
        }
    }
    for (int l = 0; l < lists; l++) {
        int n = s->num_ref_idx[l];
        struct pic **list = s->list[l];
        int first_empty = n;
        for (int i = 0; i <= n; i++)
            list[i] = i < count && i < n ? init[l][i] : NULL;
        if (count < n)
            first_empty = count;
        if (c->dep_sps) {
            // G.8.2.1: the inter-view references follow
            int a = s->anchor ? 1 : 0;
            int pos = first_empty;
            for (int j = 0; j < c->dep_sps->refs_count[a][l] && pos < n; j++) {
                bool exists = c->interview && c->dep_sps->refs[a][l][j] == c->base_view_id;
                list[pos++] = exists ? c->interview : NULL;
            }
        }
        int idx = 0, pred = c->frame_num, view_pred = -1;
        for (int m = 0; m < s->nmods[l]; m++) {
            int idc = s->mods[l][m].idc;
            uint32_t val = s->mods[l][m].val;
            if (idx > n)
                return "too many reference list modifications";
            if (idc == 0 || idc == 1) {
                // G.8.2.2.1
                int64_t diff = (int64_t)val + 1;
                if (diff > v->max_frame_num)
                    return "bad abs_diff_pic_num";
                int64_t nowrap = idc == 0 ? pred - diff : pred + diff;
                if (nowrap < 0)
                    nowrap += v->max_frame_num;
                else if (nowrap >= v->max_frame_num)
                    nowrap -= v->max_frame_num;
                pred = (int)nowrap;
                int target = nowrap > c->frame_num ? (int)nowrap - v->max_frame_num : (int)nowrap;
                struct pic *p = NULL;
                for (int i = 0; i < count; i++) {
                    if (pic_num(v->refs[i], c->frame_num, v->max_frame_num) == target)
                        p = v->refs[i];
                }
                if (!p)
                    return "reference list names a missing picture";
                list_insert(list, n, &idx, p);
            } else if (idc == 2) {
                return "reference list names a long-term picture";
            } else if (c->dep_sps) {
                // G.8.2.2.3
                int a = s->anchor ? 1 : 0;
                int max_idx = c->dep_sps->refs_count[a][l] - 1;
                int64_t vi = idc == 4 ? view_pred - ((int64_t)val + 1)
                                      : view_pred + ((int64_t)val + 1);
                if (idc == 4 && vi < 0)
                    vi += max_idx + 1;
                else if (idc == 5 && vi >= max_idx + 1)
                    vi -= max_idx + 1;
                if (vi < 0 || vi > max_idx)
                    return "bad inter-view reference index";
                view_pred = (int)vi;
                if (!c->interview || c->dep_sps->refs[a][l][vi] != c->base_view_id)
                    return "missing inter-view reference";
                list_insert(list, n, &idx, c->interview);
            }
        }
        list[n] = NULL;
    }
    return NULL;
}

static bool remove_ref(struct view_state *v, struct pic *p)
{
    for (int i = 0; i < v->nrefs; i++) {
        if (v->refs[i] == p) {
            v->refs[i] = v->refs[--v->nrefs];
            p->view_ref = false;
            return true;
        }
    }
    return false;
}

// G.8.3 / 8.2.5 for a frame of one view. removed[] gets the pictures its
// marking drops from the view.
static const char *mark(struct view_state *v, const struct sps *sps,
                        const struct slice *s, struct pic *cur,
                        struct pic **removed, int *nremoved)
{
    *nremoved = 0;
    if (!s->nal_ref_idc)
        return NULL;
    int max = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
    if (s->idr) {
        while (v->nrefs) {
            removed[(*nremoved)++] = v->refs[0];
            remove_ref(v, v->refs[0]);
        }
    } else if (s->adaptive) {
        for (int i = 0; i < s->nmmco; i++) {
            const struct mmco *m = &s->mmco[i];
            if (m->op != 1)
                continue;  // 4 changes nothing without long-term pictures
            int64_t target = (int64_t)s->frame_num - ((int64_t)m->a + 1);
            struct pic *p = NULL;
            for (int j = 0; j < v->nrefs; j++) {
                if (pic_num(v->refs[j], s->frame_num, v->max_frame_num) == target)
                    p = v->refs[j];
            }
            if (!p)
                return "marking names a missing picture";
            remove_ref(v, p);
            removed[(*nremoved)++] = p;
        }
    } else if (v->nrefs >= max) {
        // Sliding window: the smallest FrameNumWrap goes.
        struct pic *oldest = NULL;
        int oldest_num = 0;
        for (int j = 0; j < v->nrefs; j++) {
            int pn = pic_num(v->refs[j], s->frame_num, v->max_frame_num);
            if (!oldest || pn < oldest_num) {
                oldest = v->refs[j];
                oldest_num = pn;
            }
        }
        remove_ref(v, oldest);
        removed[(*nremoved)++] = oldest;
    }
    if (v->nrefs >= max)
        return "more reference pictures than the SPS allows";
    v->refs[v->nrefs++] = cur;
    cur->view_ref = true;
    v->prev_ref_frame_num = s->frame_num;
    return NULL;
}

/* Access units */

static struct pic *new_pic(struct mvc_rewriter *r)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!r->pool[i].used) {
            r->pool[i] = (struct pic){.used = true};
            return &r->pool[i];
        }
    }
    return NULL;
}

static void release_unused(struct mvc_rewriter *r)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        struct pic *p = &r->pool[i];
        if (p->used && !p->view_ref && !p->merged_ref)
            p->used = false;
    }
}

static struct slice *add_slice(struct picture_in *in)
{
    if (in->count == in->cap) {
        int cap = in->cap ? in->cap * 2 : 16;
        struct slice *s = realloc(in->slices, cap * sizeof(*s));
        if (!s)
            return NULL;
        in->slices = s;
        in->cap = cap;
    }
    struct slice *s = &in->slices[in->count++];
    memset(s, 0, offsetof(struct slice, list));
    return s;
}

static int merged_ref_count(struct mvc_rewriter *r)
{
    int n = 0;
    for (int i = 0; i < POOL_SIZE; i++)
        n += r->pool[i].used && r->pool[i].merged_ref;
    return n;
}

struct out_ctx {
    int mfn;
    int32_t out_poc;
    bool idr;
    int nal_ref_idc;  // 0 when not a reference in the rewritten stream
    struct pic *removals[MAX_REMOVALS];
    int nremovals;
    struct pic *standin;  // replaces the inter-view reference
    struct pic *interview;
};

static int merged_pic_num(const struct pic *p, int curr_mfn)
{
    return p->mfn > curr_mfn ? p->mfn - MERGED_MAX_FRAME_NUM : p->mfn;
}

static void write_list_mods(struct bitwriter *bw, struct pic **list, int n,
                            const struct out_ctx *o)
{
    if (!n || !list[0]) {
        bw_u(bw, 1, 0);
        return;
    }
    bw_u(bw, 1, 1);
    int pred = o->mfn;
    for (int i = 0; i < n && list[i]; i++) {
        struct pic *p = list[i];
        if (p == o->interview && o->standin)
            p = o->standin;
        if (p->long_term) {
            bw_ue(bw, 2);
            bw_ue(bw, 0);
            continue;
        }
        int pn = merged_pic_num(p, o->mfn);
        int nowrap = pn < 0 ? pn + MERGED_MAX_FRAME_NUM : pn;
        int diff = nowrap - pred;
        if (diff < 0) {
            bw_ue(bw, 0);
            bw_ue(bw, -diff - 1);
        } else if (diff > 0) {
            bw_ue(bw, 1);
            bw_ue(bw, diff - 1);
        } else {
            // The same picture twice in a row: a full turn back.
            bw_ue(bw, 0);
            bw_ue(bw, MERGED_MAX_FRAME_NUM - 1);
        }
        pred = nowrap;
    }
    bw_ue(bw, 3);
}

static void write_marking(struct bitwriter *bw, const struct out_ctx *o,
                          const struct slice *s)
{
    if (o->idr) {
        bw_u(bw, 1, s ? s->no_output_of_prior_pics : 0);
        bw_u(bw, 1, 0);
        return;
    }
    bw_u(bw, 1, 1);
    for (int i = 0; i < o->nremovals; i++) {
        const struct pic *p = o->removals[i];
        if (p->long_term) {
            bw_ue(bw, 2);
            bw_ue(bw, 0);
        } else {
            bw_ue(bw, 1);
            bw_ue(bw, o->mfn - merged_pic_num(p, o->mfn) - 1);
        }
    }
    bw_ue(bw, 0);
}

// Slice header up to the slice data. bottom_bits: length of the
// delta_pic_order_cnt_bottom written (0 when the PPS has none; 1, 3, 5 or 7
// for the values 0, 1, 2 or 4, all leaving the frame's order count as is).
static void write_header(struct bitwriter *bw, const struct slice *s, int pps_id,
                         int bottom_bits, const struct out_ctx *o)
{
    static const int bottom_value[8] = {0, 0, 0, 1, 0, 2, 0, 4};
    bw->pos = 0;
    bw_ue(bw, s->first_mb);
    bw_ue(bw, s->raw_type);
    bw_ue(bw, pps_id);
    bw_u(bw, MERGED_LOG2_MAX_FRAME_NUM, o->mfn);
    if (o->idr)
        bw_ue(bw, s->idr_pic_id);
    bw_u(bw, MERGED_LOG2_MAX_POC_LSB, (uint32_t)o->out_poc & 0xffff);
    if (bottom_bits)
        bw_se(bw, bottom_value[bottom_bits]);
    if (s->type == SLICE_B)
        bw_u(bw, 1, s->direct_spatial);
    if (s->type == SLICE_P || s->type == SLICE_B) {
        bw_u(bw, 1, 1);
        bw_ue(bw, s->num_ref_idx[0] - 1);
        if (s->type == SLICE_B)
            bw_ue(bw, s->num_ref_idx[1] - 1);
        write_list_mods(bw, (struct pic **)s->list[0], s->num_ref_idx[0], o);
        if (s->type == SLICE_B)
            write_list_mods(bw, (struct pic **)s->list[1], s->num_ref_idx[1], o);
    }
    bw_copy(bw, s->rbsp, s->wpt_start, s->wpt_end - s->wpt_start);
    if (o->nal_ref_idc)
        write_marking(bw, o, s);
    bw_copy(bw, s->rbsp, s->tail_start, s->header_end - s->tail_start);
}

static bool write_slice(struct mvc_rewriter *r, const struct slice *s, int view,
                        const struct out_ctx *o)
{
    struct bitwriter *bw = &r->bw;
    bw_start(r);
    bw_reserve(bw, s->rbsp_size + 64);
    int id = r->map_of[view][s->pps_id];
    const struct pps_map *m = &r->map[id];
    if (s->pps->cabac) {
        write_header(bw, s, id, m->bottom, o);
        while (bw->pos & 7)
            bw_u(bw, 1, 1);
        size_t start = (s->header_end + 7) / 8;
        bw_copy(bw, s->rbsp, start * 8, (s->rbsp_size - start) * 8);
    } else {
        // CAVLC slice data is copied bit for bit, and I_PCM samples are
        // byte-aligned within the RBSP: the new header must end at the same
        // bit of a byte as the old one. The bottom field order count (odd
        // lengths, or absent in the copies) and the copies' PPS ids (9 to 15
        // bits) reach every remainder.
        write_header(bw, s, id, 1, o);
        int fixed = (int)bw->pos - ue_bits(id) - 1;
        int adj = (int)(((s->header_end - fixed) % 8 + 8) % 8);
        int bottom = ((adj - ue_bits(id)) % 8 + 8) % 8;
        if (bottom & 1) {
            write_header(bw, s, id, bottom, o);
        } else {
            int k = (adj - 1) / 2;  // ids of 9, 11, 13, 15 bits: 1, 3, 5, 7 mod 8
            write_header(bw, s, m->variants[k], 0, o);
        }
        bw_copy(bw, s->rbsp, s->header_end, s->stop_bit - s->header_end);
        bw_trailing(bw);
    }
    return emit_nal(r, o->nal_ref_idc, o->idr ? 5 : 1);
}

static bool write_aud(struct mvc_rewriter *r, int primary_pic_type)
{
    bw_start(r);
    bw_u(&r->bw, 3, primary_pic_type);
    bw_trailing(&r->bw);
    return emit_nal(r, 0, 9);
}

static int pic_type_of(const struct picture_in *in)
{
    int t = 0;
    for (int i = 0; i < in->count; i++) {
        int st = in->slices[i].type;
        if (st == SLICE_B)
            t = 2;
        else if (st == SLICE_P && t < 1)
            t = 1;
    }
    return t;
}

// rewrite_au reserves room for an access unit's pictures beforehand.
static void begin_picture(struct mvc_rewriter *r, enum mvc_slot slot, bool idr,
                          int32_t poc)
{
    r->pics[r->npics++] = (struct out_pic){
        .slot = slot, .start = r->out_size, .idr = idr, .poc = poc, .tag = r->tag};
}

static void end_picture(struct mvc_rewriter *r)
{
    struct out_pic *op = &r->pics[r->npics - 1];
    op->size = r->out_size - op->start;
}

static bool write_standin(struct mvc_rewriter *r, struct out_ctx *o)
{
    struct bitwriter *bw = &r->bw;
    if (!r->standin_pps_emitted && !emit_standin_pps(r))
        return false;
    bw_start(r);
    int mbs = r->base_sps.width_mbs * r->base_sps.height_mbs;
    bw_ue(bw, 0);                // first_mb_in_slice
    bw_ue(bw, 5);                // P, all slices
    bw_ue(bw, r->standin_pps);
    bw_u(bw, MERGED_LOG2_MAX_FRAME_NUM, o->mfn);
    bw_u(bw, MERGED_LOG2_MAX_POC_LSB, (uint32_t)o->out_poc & 0xffff);
    bw_u(bw, 1, 1);              // num_ref_idx_active_override_flag
    bw_ue(bw, 0);
    struct pic *list[1] = {o->interview};
    struct out_ctx tmp = *o;
    tmp.standin = NULL;
    write_list_mods(bw, list, 1, &tmp);
    // dec_ref_pic_marking: long-term index 0 for itself
    bw_u(bw, 1, 1);
    if (!r->lt_enabled) {
        bw_ue(bw, 4);
        bw_ue(bw, 1);
        r->lt_enabled = true;
    }
    bw_ue(bw, 6);
    bw_ue(bw, 0);
    bw_ue(bw, 0);
    bw_se(bw, 0);                // slice_qp_delta
    bw_ue(bw, 1);                // disable_deblocking_filter_idc
    bw_ue(bw, mbs);              // mb_skip_run: the whole picture
    bw_trailing(bw);
    return emit_nal(r, 1, 1);
}

// A picture's order count as written in a layout.
static int32_t layout_poc(const struct mvc_rewriter *r, const struct pic *p,
                          enum poc_mode m)
{
    int32_t rel = p->poc - r->idr_poc;
    return m == POC_EXACT ? rel + (p->view == 1) : 4 * rel + 2 * (p->view == 1);
}

static int clip(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int64_t clip64(int64_t v)
{
    return v < -1000 ? -1000 : v > 1000 ? 1000 : v;
}

// What temporal direct (8.4.1.2.3) or implicit weights (8.4.2.3) make of a
// pair of references: a DistScaleFactor, the picture's own motion (INT_MIN)
// or a weight for list 0.
static int scaled(int32_t cur, int32_t poc0, int32_t poc1, bool implicit)
{
    int64_t d = (int64_t)poc1 - poc0;
    if (!d)
        return implicit ? 32 : INT_MIN;
    int tb = clip((int)clip64((int64_t)cur - poc0), -128, 127);
    int td = clip((int)clip64(d), -128, 127);
    int tx = (16384 + abs(td / 2)) / td;
    int dsf = clip((tb * tx + 32) >> 6, -1024, 1023);
    if (!implicit)
        return dsf;
    int w = dsf >> 2;
    return w < -64 || w > 128 ? 32 : 64 - w;
}

// Whether a B slice's temporal direct and implicit weights come out as the
// MVC decoder has them with a layout's order counts. Temporal direct only
// meets the references of its own view in list 0.
static bool distances_kept(const struct mvc_rewriter *r, const struct slice *s,
                           const struct pic *cur, const struct pic *interview,
                           enum poc_mode m)
{
    for (int implicit = 0; implicit < 2; implicit++) {
        if (implicit ? s->pps->weighted_bipred_idc != 2 : s->direct_spatial)
            continue;
        for (int i = 0; i < s->num_ref_idx[0]; i++) {
            const struct pic *p0 = s->list[0][i];
            if (!p0 || (!implicit && p0 == interview))
                continue;
            for (int j = 0; j < (implicit ? s->num_ref_idx[1] : 1); j++) {
                const struct pic *p1 = s->list[1][j];
                if (p1 && scaled(cur->poc, p0->poc, p1->poc, implicit) !=
                              scaled(layout_poc(r, cur, m), layout_poc(r, p0, m),
                                     layout_poc(r, p1, m), implicit))
                    return false;
            }
        }
    }
    return true;
}

static enum mvc_status fail(struct mvc_rewriter *r, enum mvc_status st, const char *why)
{
    r->reason = why;
    if (st == MVC_UNSUPPORTED) {
        r->unsupported = true;
    } else {
        r->waiting = true;
        clear_pictures(r);
    }
    return st;
}

static const char *check_slice_tools(const struct slice *s)
{
    const struct pps *p = s->pps;
    if (p->num_slice_groups > 1)
        return "slice groups";
    if (p->redundant_pic_cnt)
        return "redundant pictures";
    if (s->type == SLICE_SP || s->type == SLICE_SI)
        return "SP/SI slices";
    if (s->long_term_reference)
        return "long-term reference pictures";
    for (int i = 0; i < s->nmmco; i++) {
        if (s->mmco[i].op == 5)
            return "memory_management_control_operation 5";
        if (s->mmco[i].op != 1 && s->mmco[i].op != 4)
            return "long-term reference pictures";
    }
    return NULL;
}

// Checks that a picture's slices agree; returns NULL when they do.
static const char *check_picture(const struct picture_in *in)
{
    const struct slice *f = &in->slices[0];
    if (f->first_mb != 0)
        return "picture's first slice missing";
    for (int i = 1; i < in->count; i++) {
        const struct slice *s = &in->slices[i];
        if (s->frame_num != f->frame_num || s->idr != f->idr ||
            !s->nal_ref_idc != !f->nal_ref_idc || s->poc_lsb != f->poc_lsb ||
            s->delta_poc[0] != f->delta_poc[0] || s->sps != f->sps ||
            s->first_mb <= in->slices[i - 1].first_mb)
            return "slices of one picture disagree";
    }
    return NULL;
}

static enum mvc_status rewrite_au(struct mvc_rewriter *r)
{
    struct picture_in *bin = &r->in[0], *din = &r->in[1];
    const char *err;
    if (!bin->count && !din->count)
        return MVC_OK;
    if (!bin->count)
        return fail(r, MVC_DROPPED, "access unit without a base view picture");
    if (!din->count)
        return fail(r, MVC_DROPPED, "access unit without a dependent view picture");
    for (int v = 0; v < 2; v++) {
        if ((err = check_picture(&r->in[v])))
            return fail(r, MVC_DROPPED, err);
    }
    const struct slice *b0 = &bin->slices[0], *d0 = &din->slices[0];
    if (b0->idr != d0->idr)
        return fail(r, MVC_DROPPED, "views disagree on IDR");
    if (r->npics + MAX_PICTURES_PER_AU > r->pics_cap) {
        int cap = r->pics_cap ? 2 * r->pics_cap : 64;
        struct out_pic *p = realloc(r->pics, cap * sizeof(*p));
        if (!p)
            return fail(r, MVC_NO_MEMORY, no_memory);
        r->pics = p;
        r->pics_cap = cap;
    }
    if (r->waiting && !b0->idr) {
        r->reason = "waiting for an IDR access unit";
        return MVC_DROPPED;
    }
    if (b0->idr) {
        if ((err = activate(r, b0->sps, d0->sps)))
            return fail(r, MVC_UNSUPPORTED, err);
        free(r->base_sps.rbsp);
        r->base_sps = *b0->sps;
        r->base_sps.rbsp = malloc(b0->sps->rbsp_size);
        if (!r->base_sps.rbsp)
            return fail(r, MVC_NO_MEMORY, no_memory);
        memcpy(r->base_sps.rbsp, b0->sps->rbsp, b0->sps->rbsp_size);
        r->dep_sps = *d0->sps;
        r->dep_sps.rbsp = NULL;
        r->base_sps_gen = r->sps_gen[b0->sps->id];
        r->dep_sps_gen = r->ssps_gen[d0->sps->id];
        r->base_view_id = r->dep_sps.view_id[0];
        clear_pictures(r);
        r->views[0].max_frame_num = 1 << r->base_sps.log2_max_frame_num;
        r->views[1].max_frame_num = 1 << r->dep_sps.log2_max_frame_num;
        r->waiting = false;
            for (int i = 0; i < 256; i++)
            r->map[i].emitted = false;
        r->standin_pps_emitted = false;
    } else if (b0->sps->id != r->base_sps.id || d0->sps->id != r->dep_sps.id ||
               r->sps_gen[b0->sps->id] != r->base_sps_gen ||
               r->ssps_gen[d0->sps->id] != r->dep_sps_gen) {
        return fail(r, MVC_DROPPED, "SPS changed outside an IDR");
    }
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < r->in[v].count; i++) {
            const struct slice *s = &r->in[v].slices[i];
            if ((err = check_slice_tools(s)))
                return fail(r, MVC_UNSUPPORTED, err);
            if (map_pps(r, v, s->pps_id) < 0)
                return fail(r, MVC_UNSUPPORTED, "too many PPSs");
        }
    }

    // Order counts and frame numbers, per view
    const struct sps *vsps[2] = {&r->base_sps, &r->dep_sps};
    int32_t poc[2];
    for (int v = 0; v < 2; v++) {
        struct view_state *vs = &r->views[v];
        const struct slice *s = &r->in[v].slices[0];
        if (!s->idr && s->frame_num != (vs->prev_ref_frame_num + 1) % vs->max_frame_num)
            return fail(r, MVC_DROPPED, "frame_num gap");
        if (!compute_poc(vs, vsps[v], s, &poc[v]))
            return fail(r, MVC_UNSUPPORTED, "picture order count out of range");
    }
    if (poc[0] != poc[1])
        return fail(r, MVC_DROPPED, "views disagree on picture order count");
    if (b0->idr)
        r->idr_poc = poc[0];

    struct pic *base = new_pic(r), *dep = new_pic(r);
    if (!base || !dep)
        return fail(r, MVC_UNSUPPORTED, "too many pictures held");
    base->view = 0;
    base->frame_num = b0->frame_num;
    base->poc = poc[0];
    dep->view = 1;
    dep->frame_num = d0->frame_num;
    dep->poc = poc[1];

    // Lists before marking (8.2.4 runs at slice start, 8.2.5 after decoding)
    struct list_ctx lc = {.v = &r->views[0], .frame_num = b0->frame_num, .poc = poc[0]};
    for (int i = 0; i < bin->count; i++) {
        if ((err = build_lists(&lc, &bin->slices[i])))
            return fail(r, MVC_DROPPED, err);
    }
    struct pic *removed[2][MAX_REF_FRAMES + 1];
    int nremoved[2];
    if ((err = mark(&r->views[0], &r->base_sps, b0, base, removed[0], &nremoved[0])))
        return fail(r, MVC_DROPPED, err);

    lc = (struct list_ctx){.v = &r->views[1], .frame_num = d0->frame_num, .poc = poc[1],
                           .dep_sps = &r->dep_sps, .base_view_id = r->base_view_id,
                           .interview = b0->inter_view ? base : NULL};
    for (int i = 0; i < din->count; i++) {
        if ((err = build_lists(&lc, &din->slices[i])))
            return fail(r, MVC_DROPPED, err);
    }
    if ((err = mark(&r->views[1], &r->dep_sps, d0, dep, removed[1], &nremoved[1])))
        return fail(r, MVC_DROPPED, err);

    // Does the dependent view predict from the base picture, and does it
    // need the hidden copy?
    bool interview_used = false, colocated = false, base_intra = true;
    for (int i = 0; i < bin->count; i++)
        base_intra &= bin->slices[i].type == SLICE_I;
    for (int i = 0; i < din->count; i++) {
        const struct slice *s = &din->slices[i];
        for (int l = 0; l < 2; l++) {
            for (int k = 0; k < s->num_ref_idx[l]; k++)
                interview_used |= s->list[l][k] == base;
        }
        if (s->type == SLICE_B && s->list[1][0] == base)
            colocated = true;
    }
    bool use_standin = colocated && !base_intra;

    // Temporal direct and implicit weights depend on order count distances.
    // Each layout is checked against what the MVC decoder computes; EXACT
    // has no room for hidden copies or odd order counts either.
    int32_t rel = poc[0] - r->idr_poc;
    bool spread_ok = true, exact_ok = !use_standin && !(rel & 1);
    r->au_plain_b = false;
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < r->in[v].count; i++) {
            const struct slice *s = &r->in[v].slices[i];
            if (s->type != SLICE_B)
                continue;
            bool implicit = s->pps->weighted_bipred_idc == 2;
            if (s->direct_spatial && !implicit) {
                r->au_plain_b = true;
                continue;
            }
            for (int l = 0; v && implicit && l < 2; l++) {
                for (int k = 0; k < s->num_ref_idx[l]; k++) {
                    if (s->list[l][k] == base)
                        return fail(r, MVC_UNSUPPORTED,
                                    "implicit weighted prediction from the other view");
                }
            }
            if (!s->direct_spatial && s->list[1][0] == base)
                return fail(r, MVC_UNSUPPORTED, "temporal direct from the other view");
            struct pic *cur = v ? dep : base;
            spread_ok &= distances_kept(r, s, cur, base, POC_SPREAD);
            exact_ok &= distances_kept(r, s, cur, base, POC_EXACT);
        }
    }
    r->period_exact |= !spread_ok;
    r->period_spread |= !exact_ok;
    r->seen_exact |= !spread_ok;
    if (r->period_exact && r->period_spread)
        return fail(r, MVC_UNSUPPORTED,
                    "temporal direct or implicit weights with odd order counts or hidden copies");
    if ((r->poc_mode == POC_EXACT && !exact_ok) || (r->poc_mode == POC_SPREAD && !spread_ok)) {
        r->reason = "picture order count layout changed: resyncing at the next IDR";
        return LAYOUT_MISMATCH;
    }
    int32_t out_base = r->poc_mode == POC_EXACT ? rel : 4 * rel;

    // Base picture
    struct out_ctx o = {.idr = b0->idr, .interview = base};
    o.mfn = b0->idr ? 0 : (r->prev_ref_mfn + 1) % MERGED_MAX_FRAME_NUM;
    o.out_poc = out_base;
    // The decoder derives the order count from 16 bits relative to the
    // previous reference picture (8.2.1.1).
    int64_t step = (int64_t)o.out_poc - (b0->idr ? 0 : r->prev_ref_out_poc);
    if (step <= -32760 || step >= 32760)
        return fail(r, MVC_UNSUPPORTED, "picture order count steps too large");
    bool base_ref = base->view_ref || interview_used;
    o.nal_ref_idc = base_ref ? (b0->nal_ref_idc ? b0->nal_ref_idc : 1) : 0;
    if (base_ref) {
        for (int i = 0; i < nremoved[0]; i++) {
            if (removed[0][i]->merged_ref)
                o.removals[o.nremovals++] = removed[0][i];
        }
        for (int i = 0; i < r->npending; i++)
            o.removals[o.nremovals++] = r->pending[i];
        r->npending = 0;
    } else {
        // Removals wait for the next reference picture.
        for (int i = 0; i < nremoved[0]; i++) {
            if (removed[0][i]->merged_ref && r->npending < MAX_REMOVALS)
                r->pending[r->npending++] = removed[0][i];
        }
    }
    base->mfn = o.mfn;
    begin_picture(r, MVC_SLOT_BASE, b0->idr, o.out_poc);
    bool ok = write_aud(r, pic_type_of(bin));
    if (ok && b0->idr) {
        write_sps(r);
        ok = emit_nal(r, 3, 7);
    }
    if (ok && (err = write_needed_pps(r, bin, 0)))
        return fail(r, err == no_memory ? MVC_NO_MEMORY : MVC_UNSUPPORTED, err);
    for (int i = 0; ok && i < bin->count; i++)
        ok = write_slice(r, &bin->slices[i], 0, &o);
    end_picture(r);
    if (!ok)
        return fail(r, MVC_NO_MEMORY, no_memory);
    for (int i = 0; i < o.nremovals; i++)
        o.removals[i]->merged_ref = false;
    base->merged_ref = base_ref;
    if (base_ref) {
        r->prev_ref_mfn = o.mfn;
        r->prev_ref_out_poc = o.out_poc;
        if (merged_ref_count(r) > r->merged_refs_max)
            return fail(r, MVC_UNSUPPORTED, "too many reference pictures");
    }

    // Hidden copy
    struct pic *standin = NULL;
    if (use_standin) {
        if (r->standin_pps < 0) {
            for (int i = 255; i >= 0 && r->standin_pps < 0; i--) {
                if (!r->map[i].used)
                    r->standin_pps = i;
            }
            if (r->standin_pps < 0)
                return fail(r, MVC_UNSUPPORTED, "too many PPSs");
        }
        standin = new_pic(r);
        if (!standin)
            return fail(r, MVC_UNSUPPORTED, "too many pictures held");
        standin->view = 2;
        standin->poc = poc[0];
        struct out_ctx so = {.interview = base};
        so.mfn = (r->prev_ref_mfn + 1) % MERGED_MAX_FRAME_NUM;
        so.out_poc = o.out_poc + 1;
        standin->mfn = so.mfn;
        begin_picture(r, MVC_SLOT_HIDDEN, false, so.out_poc);
        ok = write_aud(r, 1) && write_standin(r, &so);
        end_picture(r);
        if (!ok)
            return fail(r, MVC_NO_MEMORY, no_memory);
        standin->merged_ref = true;
        standin->long_term = true;
        r->prev_ref_mfn = so.mfn;
        r->prev_ref_out_poc = so.out_poc;
        if (merged_ref_count(r) > r->merged_refs_max)
            return fail(r, MVC_UNSUPPORTED, "too many reference pictures");
    }

    // Dependent picture
    o = (struct out_ctx){.interview = base, .standin = standin};
    o.mfn = (r->prev_ref_mfn + 1) % MERGED_MAX_FRAME_NUM;
    o.out_poc = out_base + (r->poc_mode == POC_EXACT ? 1 : 2);
    for (int i = 0; i < nremoved[1]; i++) {
        if (removed[1][i]->merged_ref)
            o.removals[o.nremovals++] = removed[1][i];
    }
    if (base->merged_ref && !base->view_ref)
        o.removals[o.nremovals++] = base;
    if (standin)
        o.removals[o.nremovals++] = standin;
    for (int i = 0; i < r->npending; i++)
        o.removals[o.nremovals++] = r->pending[i];
    bool dep_ref = dep->view_ref || o.nremovals;
    if (dep_ref) {
        r->npending = 0;
    } else {
        o.nremovals = 0;
    }
    o.nal_ref_idc = dep_ref ? (d0->nal_ref_idc ? d0->nal_ref_idc : 1) : 0;
    dep->mfn = o.mfn;
    begin_picture(r, MVC_SLOT_DEPENDENT, false, o.out_poc);
    ok = write_aud(r, pic_type_of(din));
    if (ok && (err = write_needed_pps(r, din, 1)))
        return fail(r, err == no_memory ? MVC_NO_MEMORY : MVC_UNSUPPORTED, err);
    for (int i = 0; ok && i < din->count; i++)
        ok = write_slice(r, &din->slices[i], 1, &o);
    end_picture(r);
    if (!ok)
        return fail(r, MVC_NO_MEMORY, no_memory);
    for (int i = 0; i < o.nremovals; i++) {
        o.removals[i]->merged_ref = false;
        o.removals[i]->long_term = false;
    }
    dep->merged_ref = dep_ref;
    if (dep_ref) {
        r->prev_ref_mfn = o.mfn;
        r->prev_ref_out_poc = o.out_poc;
        if (!dep->view_ref)
            r->pending[r->npending++] = dep;
        if (merged_ref_count(r) > r->merged_refs_max)
            return fail(r, MVC_UNSUPPORTED, "too many reference pictures");
    }
    // A base picture held for inter-view prediction but not removed yet
    // (the dependent picture couldn't carry marking) is removed later.
    release_unused(r);
    return MVC_OK;
}

static const char *add_vcl(struct mvc_rewriter *r, const uint8_t *rbsp, size_t size,
                           int type, int ref_idc, const uint8_t *prefix)
{
    struct slice tmp;
    memset(&tmp, 0, offsetof(struct slice, list));
    int view = 0;
    const uint8_t *hdr = rbsp;
    if (type == 20) {
        if (size < 3)
            return "truncated NAL unit header";
        if (rbsp[0] & 0x80)
            return NULL;  // SVC
        hdr = rbsp + 3;
        size -= 3;
        tmp.idr = !(rbsp[0] & 0x40);
        tmp.view_id = ((rbsp[1] << 2) | (rbsp[2] >> 6)) & 1023;
        tmp.anchor = (rbsp[2] >> 2) & 1;
        tmp.inter_view = (rbsp[2] >> 1) & 1;
        view = 1;
    } else {
        tmp.idr = type == 5;
        tmp.view_id = 0;
        tmp.inter_view = true;
        if (prefix) {
            tmp.view_id = ((prefix[1] << 2) | (prefix[2] >> 6)) & 1023;
            tmp.anchor = (prefix[2] >> 2) & 1;
            tmp.inter_view = (prefix[2] >> 1) & 1;
        }
    }
    const char *err = parse_slice_header(&tmp, hdr, size, type, ref_idc, r->pps,
                                         view ? r->ssps : r->sps);
    if (err)
        return err;
    if (view) {
        // Only the view of view order index 1 is kept.
        const struct sps *ss = tmp.sps;
        if (ss->num_views < 2)
            return "dependent slice without an MVC subset SPS";
        if (tmp.view_id != ss->view_id[1]) {
            for (int i = 0; i < 2; i++) {
                if (tmp.view_id == ss->view_id[i])
                    return "dependent slice in the base view";
            }
            return NULL;
        }
    }
    struct slice *s = add_slice(&r->in[view]);
    if (!s)
        return no_memory;
    memcpy(s, &tmp, offsetof(struct slice, list));
    return NULL;
}

// First 00 00 01 at or after p (or also 00 00 00, which ends a NAL unit
// too); end when there is none.
static const uint8_t *find_zero_pair(const uint8_t *p, const uint8_t *end,
                                     bool or_zero)
{
    while (end - p >= 3) {
        const uint8_t *z = memchr(p, 0, end - p - 2);
        if (!z)
            break;
        if (!z[1] && (z[2] == 1 || (or_zero && !z[2])))
            return z;
        p = z + 1;
    }
    return end;
}

// Splits an access unit into NAL units, stores parameter sets and parses the
// slices of both views into r->in.
static enum mvc_status parse_au(struct mvc_rewriter *r, const uint8_t *data, size_t size)
{
    r->in[0].count = r->in[1].count = 0;
    if (size + 16 > r->rbsp_cap) {
        uint8_t *p = realloc(r->rbsp, size + 16);
        if (!p)
            return fail(r, MVC_NO_MEMORY, no_memory);
        r->rbsp = p;
        r->rbsp_cap = size + 16;
    }
    r->rbsp_used = 0;

    const char *damage = NULL;
    bool unsupported = false;
    uint8_t prefix[3];
    bool have_prefix = false;
    size_t pos = 0;
    while (pos < size) {
        const uint8_t *nal;
        size_t n;
        if (r->length_size) {
            if (size - pos < (size_t)r->length_size)
                break;
            n = 0;
            for (int i = 0; i < r->length_size; i++)
                n = (n << 8) | data[pos + i];
            pos += r->length_size;
            if (n > size - pos) {
                damage = "truncated NAL unit";
                break;
            }
            nal = data + pos;
            pos += n;
        } else {
            // Find the next start code, then the end of this NAL unit.
            const uint8_t *end = data + size;
            const uint8_t *p = find_zero_pair(data + pos, end, false);
            if (p + 3 > end)
                break;
            nal = p + 3;
            const uint8_t *q = find_zero_pair(nal, end, true);
            n = q - nal;
            pos = q - data;
            while (n && !nal[n - 1])
                n--;
        }
        if (!n)
            continue;
        if (nal[0] & 0x80) {
            damage = "forbidden_zero_bit set";
            continue;
        }
        int type = nal[0] & 31, ref_idc = nal[0] >> 5;
        bool was_prefix = have_prefix;
        have_prefix = false;
        if (type != 1 && type != 5 && type != 7 && type != 8 && type != 14 &&
            type != 15 && type != 20) {
            if (type >= 2 && type <= 4)
                unsupported = true;
            continue;
        }
        uint8_t *rbsp = r->rbsp + r->rbsp_used;
        size_t rsize = unescape(rbsp, nal + 1, n - 1);
        r->rbsp_used += rsize;
        const char *err = NULL;
        switch (type) {
        case 7:
        case 15:
            err = store_sps(r, rbsp, rsize, type == 15);
            break;
        case 8:
            err = store_pps(r, rbsp, rsize);
            break;
        case 14:
            if (rsize >= 3 && !(rbsp[0] & 0x80)) {
                memcpy(prefix, rbsp, 3);
                have_prefix = true;
            }
            break;
        default:
            err = add_vcl(r, rbsp, rsize, type, ref_idc, was_prefix ? prefix : NULL);
            break;
        }
        if (err == no_memory)
            return fail(r, MVC_NO_MEMORY, no_memory);
        // A bad parameter set only matters if a picture uses it.
        if (err && type != 7 && type != 8 && type != 15 && !damage)
            damage = err;
    }
    if (unsupported)
        return fail(r, MVC_UNSUPPORTED, "slice data partitioning");
    if (damage) {
        if (r->waiting) {
            r->reason = damage;
            return MVC_DROPPED;
        }
        return fail(r, MVC_DROPPED, damage);
    }
    return MVC_OK;
}

static enum mvc_status process_au(struct mvc_rewriter *r, const uint8_t *data, size_t size)
{
    enum mvc_status st = parse_au(r, data, size);
    return st == MVC_OK ? rewrite_au(r) : st;
}

// Drops the pictures written from index from on.
static void truncate_pictures(struct mvc_rewriter *r, int from)
{
    if (from < r->npics)
        r->out_size = r->pics[from].start;
    r->npics = from;
}

// Settles the period's layout: its pictures can all be returned.
static void settle(struct mvc_rewriter *r)
{
    r->holding = false;
    r->nheld = 0;
    r->hold_size = 0;
    r->period_pic = r->npics;
}

static bool hold_au(struct mvc_rewriter *r, const uint8_t *data, size_t size)
{
    if (r->nheld == r->held_cap) {
        int cap = r->held_cap ? 2 * r->held_cap : HOLD_MAX + 1;
        struct held_au *h = realloc(r->held, cap * sizeof(*h));
        if (!h)
            return false;
        r->held = h;
        r->held_cap = cap;
    }
    if (r->hold_size + size > r->hold_cap) {
        size_t cap = r->hold_cap ? r->hold_cap : 1 << 20;
        while (cap < r->hold_size + size)
            cap *= 2;
        uint8_t *p = realloc(r->hold, cap);
        if (!p)
            return false;
        r->hold = p;
        r->hold_cap = cap;
    }
    memcpy(r->hold + r->hold_size, data, size);
    r->held[r->nheld++] = (struct held_au){r->hold_size, size, r->tag};
    r->hold_size += size;
    return true;
}

// The held access units need the other layout: rewrite them again from the
// IDR. A period that needs both was refused by rewrite_au already.
static enum mvc_status replay(struct mvc_rewriter *r)
{
    int64_t tag = r->tag;
    r->poc_mode = r->poc_mode == POC_EXACT ? POC_SPREAD : POC_EXACT;
    truncate_pictures(r, r->period_pic);
    clear_pictures(r);
    r->waiting = true;
    enum mvc_status st = MVC_OK;
    for (int i = 0; i < r->nheld && st == MVC_OK; i++) {
        int first = r->npics;
        r->tag = r->held[i].tag;
        st = process_au(r, r->hold + r->held[i].start, r->held[i].size);
        if (st == LAYOUT_MISMATCH)
            st = fail(r, MVC_UNSUPPORTED, "access units need both order count layouts");
        if (st != MVC_OK)
            truncate_pictures(r, first);
    }
    r->tag = tag;
    return st;
}

static void release(struct mvc_rewriter *r, const struct mvc_picture **out, int *count)
{
    int n = r->holding ? r->period_pic : r->npics;
    if (n > r->pub_cap) {
        int cap = n > 2 * r->pub_cap ? n : 2 * r->pub_cap;
        struct mvc_picture *p = realloc(r->pub, cap * sizeof(*p));
        if (!p) {
            // Keep them for the next call.
            *count = 0;
            return;
        }
        r->pub = p;
        r->pub_cap = cap;
    }
    for (int i = 0; i < n; i++) {
        const struct out_pic *op = &r->pics[i];
        r->pub[i] = (struct mvc_picture){
            .slot = op->slot,
            .data = r->out + op->start,
            .size = op->size,
            .idr = op->idr,
            .poc = op->poc,
            .tag = op->tag,
        };
    }
    r->nreleased = n;
    *out = r->pub;
    *count = n;
}

// Forgets the pictures the last call returned.
static void drop_released(struct mvc_rewriter *r)
{
    int n = r->nreleased;
    if (!n)
        return;
    size_t cut = n < r->npics ? r->pics[n].start : r->out_size;
    memmove(r->out, r->out + cut, r->out_size - cut);
    r->out_size -= cut;
    for (int i = n; i < r->npics; i++) {
        r->pics[i - n] = r->pics[i];
        r->pics[i - n].start -= cut;
    }
    r->npics -= n;
    r->period_pic = r->period_pic > n ? r->period_pic - n : 0;
    r->nreleased = 0;
}

enum mvc_status mvc_rewrite(struct mvc_rewriter *r, const uint8_t *data, size_t size,
                            int64_t tag, const struct mvc_picture **out, int *count)
{
    *out = NULL;
    *count = 0;
    drop_released(r);
    if (r->unsupported)
        return MVC_UNSUPPORTED;
    r->reason = NULL;
    r->tag = tag;
    enum mvc_status st = parse_au(r, data, size);
    if (st == MVC_OK && r->in[0].count && r->in[0].slices[0].idr) {
        // A new period: the last one's pictures are settled by now.
        settle(r);
        r->holding = true;
        r->period_exact = r->period_spread = false;
        r->poc_mode = r->seen_exact ? POC_EXACT : POC_SPREAD;
    }
    if (st == MVC_OK && r->holding && !hold_au(r, data, size))
        st = fail(r, MVC_NO_MEMORY, no_memory);
    int first = r->npics;
    r->au_plain_b = false;
    if (st == MVC_OK)
        st = rewrite_au(r);
    if (st == LAYOUT_MISMATCH) {
        truncate_pictures(r, first);
        if (r->holding)
            st = replay(r);
        else
            st = fail(r, MVC_DROPPED, r->reason);
    } else if (st != MVC_OK) {
        truncate_pictures(r, first);
    }
    if (st == MVC_UNSUPPORTED) {
        truncate_pictures(r, 0);
        return st;
    }
    if (st != MVC_OK) {
        // What was written before the damage stands.
        settle(r);
    } else if (r->holding) {
        // The guess is confirmed by evidence for it, or by a B picture
        // that needs nothing special (unless an earlier period needed
        // EXACT), or the hold is long enough.
        if ((r->poc_mode == POC_SPREAD ? r->period_spread : r->period_exact) ||
            (r->poc_mode == POC_SPREAD && r->au_plain_b && !r->seen_exact) ||
            r->nheld >= HOLD_MAX)
            settle(r);
    }
    release(r, out, count);
    return st;
}

void mvc_rewriter_flush(struct mvc_rewriter *r, const struct mvc_picture **out,
                        int *count)
{
    *out = NULL;
    *count = 0;
    drop_released(r);
    if (r->unsupported)
        return;
    settle(r);
    release(r, out, count);
}

static void feed_config_nals(struct mvc_rewriter *r, const uint8_t *p, size_t size,
                             size_t *pos, int count)
{
    for (int i = 0; i < count && *pos + 2 <= size; i++) {
        size_t n = (p[*pos] << 8) | p[*pos + 1];
        *pos += 2;
        if (n > size - *pos)
            return;
        const uint8_t *nal = p + *pos;
        *pos += n;
        if (!n || n - 1 > r->rbsp_cap)
            continue;
        int type = nal[0] & 31;
        size_t rsize = unescape(r->rbsp, nal + 1, n - 1);
        if (type == 7 || type == 15)
            store_sps(r, r->rbsp, rsize, type == 15);
        else if (type == 8)
            store_pps(r, r->rbsp, rsize);
    }
}

// avcC or mvcC record (ISO/IEC 14496-15): parameter sets in two arrays.
static void parse_config_record(struct mvc_rewriter *r, const uint8_t *p, size_t size,
                                bool set_length)
{
    if (size < 7 || p[0] != 1)
        return;
    if (set_length)
        r->length_size = (p[4] & 3) + 1;
    size_t pos = 6;
    feed_config_nals(r, p, size, &pos, p[5] & 31);
    if (pos < size) {
        int n = p[pos++];
        feed_config_nals(r, p, size, &pos, n);
    }
}

struct mvc_rewriter *mvc_rewriter_create(const uint8_t *codec_private, size_t size)
{
    struct mvc_rewriter *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    memset(r->map_of, -1, sizeof(r->map_of));
    r->standin_pps = -1;
    r->waiting = true;
    r->rbsp_cap = size + 16;
    r->rbsp = malloc(r->rbsp_cap);
    if (!r->rbsp) {
        free(r);
        return NULL;
    }
    if (codec_private && size >= 7 && codec_private[0] == 1) {
        parse_config_record(r, codec_private, size, true);
        // An mvcC box may follow: size, 'mvcC', record.
        for (size_t i = 4; i + 4 <= size; i++) {
            if (!memcmp(codec_private + i, "mvcC", 4)) {
                size_t box = ((size_t)codec_private[i - 4] << 24) |
                             (codec_private[i - 3] << 16) |
                             (codec_private[i - 2] << 8) | codec_private[i - 1];
                size_t avail = size - (i + 4);
                if (box >= 8 && box - 8 < avail)
                    avail = box - 8;
                parse_config_record(r, codec_private + i + 4, avail, false);
                break;
            }
        }
    } else if (codec_private && size) {
        // Annex B parameter sets
        parse_au(r, codec_private, size);
        r->reason = NULL;
        r->waiting = true;
    }
    return r;
}
