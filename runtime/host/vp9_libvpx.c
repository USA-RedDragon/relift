/**
 * @file vp9_libvpx.c
 * @brief VP9 decoding below the frame headers through libvpx (BSD license).
 *
 * libvpx decodes whole frames, headers included; a hardware decoder's guest
 * has already parsed the headers. This file compiles libvpx's own frame
 * decoder (vp9_decodeframe.c, from the libvpx tree the build points at) into
 * itself, sets the decoder state the headers would have set from the
 * frame's parameters, and runs libvpx's tile decoder and loop filter
 * (decode_tiles) on the tile data. The rest of libvpx comes from the
 * library the program links.
 */
#include <relift/video.h>

#include <vp9/decoder/vp9_decodeframe.c>

#include <stdlib.h>
#include <string.h>

enum { BUF_CUR = 0, BUF_REF0 = 1, BUF_PREV = 4 };

typedef struct lv_dec {
    BufferPool pool;
    VP9Decoder *pbi;
    MV_REF *prev_mvs;
    size_t prev_mvs_n;
} lv_dec;

static void *lv_open(void)
{
    lv_dec *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->pbi = vp9_decoder_create(&d->pool);
    if (!d->pbi) {
        free(d);
        return NULL;
    }
    d->pbi->max_threads = 1;
    d->pbi->row_mt = 0;
    d->pbi->need_resync = 0;
    return d;
}

static void lv_close(void *h)
{
    lv_dec *d = h;
    if (!d)
        return;
    for (int i = 0; i < FRAME_BUFFERS; i++) {
        vpx_free(d->pool.frame_bufs[i].mvs);
        d->pool.frame_bufs[i].mvs = NULL;
        vpx_free_frame_buffer(&d->pool.frame_bufs[i].buf);
    }
    vp9_decoder_remove(d->pbi);
    vpx_free(d->prev_mvs);
    free(d);
}

static void copy_plane(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * (size_t)dst_stride, src + (size_t)y * (size_t)src_stride, (size_t)w);
}

static void alloc_buffer(VP9_COMMON *cm, RefCntBuffer *b, int w, int h)
{
    if (vpx_realloc_frame_buffer(&b->buf, w, h, 1, 1, VP9_DEC_BORDER_IN_PIXELS, cm->byte_alignment, &b->raw_frame_buffer, NULL, NULL))
        vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR, "Failed to allocate frame buffer");
    b->buf.subsampling_x = 1;
    b->buf.subsampling_y = 1;
    b->buf.bit_depth = VPX_BITS_8;
}

static void load_ref(VP9_COMMON *cm, RefCntBuffer *b, const rl_vp9_image *im)
{
    int w = (int)im->width, h = (int)im->height;
    if (w <= 0 || h <= 0 || !im->y || !im->u || !im->v)
        vpx_internal_error(&cm->error, VPX_CODEC_CORRUPT_FRAME, "Missing reference frame");
    alloc_buffer(cm, b, w, h);
    YV12_BUFFER_CONFIG *y = &b->buf;
    copy_plane(y->y_buffer, y->y_stride, im->y, (int)im->y_stride, w, h);
    copy_plane(y->u_buffer, y->uv_stride, im->u, (int)im->uv_stride, (w + 1) / 2, (h + 1) / 2);
    copy_plane(y->v_buffer, y->uv_stride, im->v, (int)im->uv_stride, (w + 1) / 2, (h + 1) / 2);
    vpx_extend_frame_borders(y);
}

static void load_probs(FRAME_CONTEXT *fc, const rl_vp9_probs *p)
{
    memcpy(fc->y_mode_prob, p->y_mode, sizeof fc->y_mode_prob);
    memcpy(fc->uv_mode_prob, p->uv_mode, sizeof fc->uv_mode_prob);
    memcpy(fc->partition_prob, p->partition, sizeof fc->partition_prob);
    memcpy(fc->coef_probs, p->coef, sizeof fc->coef_probs);
    memcpy(fc->switchable_interp_prob, p->switchable_interp, sizeof fc->switchable_interp_prob);
    memcpy(fc->inter_mode_probs, p->inter_mode, sizeof fc->inter_mode_probs);
    memcpy(fc->intra_inter_prob, p->intra_inter, sizeof fc->intra_inter_prob);
    memcpy(fc->comp_inter_prob, p->comp_inter, sizeof fc->comp_inter_prob);
    memcpy(fc->single_ref_prob, p->single_ref, sizeof fc->single_ref_prob);
    memcpy(fc->comp_ref_prob, p->comp_ref, sizeof fc->comp_ref_prob);
    memcpy(fc->tx_probs.p32x32, p->tx_32x32, sizeof fc->tx_probs.p32x32);
    memcpy(fc->tx_probs.p16x16, p->tx_16x16, sizeof fc->tx_probs.p16x16);
    memcpy(fc->tx_probs.p8x8, p->tx_8x8, sizeof fc->tx_probs.p8x8);
    memcpy(fc->skip_probs, p->skip, sizeof fc->skip_probs);
    memcpy(fc->nmvc.joints, p->mv_joints, sizeof fc->nmvc.joints);
    for (int i = 0; i < 2; i++) {
        nmv_component *c = &fc->nmvc.comps[i];
        c->sign = p->mv_sign[i];
        memcpy(c->classes, p->mv_classes[i], sizeof c->classes);
        memcpy(c->class0, p->mv_class0[i], sizeof c->class0);
        memcpy(c->bits, p->mv_bits[i], sizeof c->bits);
        memcpy(c->class0_fp, p->mv_class0_fp[i], sizeof c->class0_fp);
        memcpy(c->fp, p->mv_fp[i], sizeof c->fp);
        c->class0_hp = p->mv_class0_hp[i];
        c->hp = p->mv_hp[i];
    }
    fc->initialized = 1;
}

static void store_counts(rl_vp9_counts *o, const FRAME_COUNTS *c)
{
    memcpy(o->y_mode, c->y_mode, sizeof o->y_mode);
    memcpy(o->uv_mode, c->uv_mode, sizeof o->uv_mode);
    memcpy(o->partition, c->partition, sizeof o->partition);
    memcpy(o->coef, c->coef, sizeof o->coef);
    memcpy(o->eob_branch, c->eob_branch, sizeof o->eob_branch);
    memcpy(o->switchable_interp, c->switchable_interp, sizeof o->switchable_interp);
    memcpy(o->inter_mode, c->inter_mode, sizeof o->inter_mode);
    memcpy(o->intra_inter, c->intra_inter, sizeof o->intra_inter);
    memcpy(o->comp_inter, c->comp_inter, sizeof o->comp_inter);
    memcpy(o->single_ref, c->single_ref, sizeof o->single_ref);
    memcpy(o->comp_ref, c->comp_ref, sizeof o->comp_ref);
    memcpy(o->tx_32x32, c->tx.p32x32, sizeof o->tx_32x32);
    memcpy(o->tx_16x16, c->tx.p16x16, sizeof o->tx_16x16);
    memcpy(o->tx_8x8, c->tx.p8x8, sizeof o->tx_8x8);
    memcpy(o->skip, c->skip, sizeof o->skip);
    memcpy(o->mv_joints, c->mv.joints, sizeof o->mv_joints);
    for (int i = 0; i < 2; i++) {
        const nmv_component_counts *m = &c->mv.comps[i];
        memcpy(o->mv_sign[i], m->sign, sizeof o->mv_sign[i]);
        memcpy(o->mv_classes[i], m->classes, sizeof o->mv_classes[i]);
        memcpy(o->mv_class0[i], m->class0, sizeof o->mv_class0[i]);
        memcpy(o->mv_bits[i], m->bits, sizeof o->mv_bits[i]);
        memcpy(o->mv_class0_fp[i], m->class0_fp, sizeof o->mv_class0_fp[i]);
        memcpy(o->mv_fp[i], m->fp, sizeof o->mv_fp[i]);
        memcpy(o->mv_class0_hp[i], m->class0_hp, sizeof o->mv_class0_hp[i]);
        memcpy(o->mv_hp[i], m->hp, sizeof o->mv_hp[i]);
    }
}

static void load_prev_mvs(VP9_COMMON *cm, lv_dec *d, const rl_vp9_mvref *in)
{
    size_t n = (size_t)cm->mi_rows * (size_t)cm->mi_cols;
    RefCntBuffer *prev = &d->pool.frame_bufs[BUF_PREV];
    if (prev->mvs == NULL || (size_t)prev->mi_rows * (size_t)prev->mi_cols < n) {
        vpx_free(prev->mvs);
        prev->mvs = vpx_calloc(n, sizeof *prev->mvs);
        if (!prev->mvs)
            vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR, "Failed to allocate motion vectors");
    }
    prev->mi_rows = cm->mi_rows;
    prev->mi_cols = cm->mi_cols;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            prev->mvs[i].mv[k].as_mv.row = in[i].mv[k][0];
            prev->mvs[i].mv[k].as_mv.col = in[i].mv[k][1];
            prev->mvs[i].ref_frame[k] = in[i].ref[k];
        }
    }
    cm->prev_frame = prev;
}

static void store_mvs(const VP9_COMMON *cm, rl_vp9_mvref *out)
{
    size_t n = (size_t)cm->mi_rows * (size_t)cm->mi_cols;
    const MV_REF *m = cm->cur_frame->mvs;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            out[i].mv[k][0] = m[i].mv[k].as_mv.row;
            out[i].mv[k][1] = m[i].mv[k].as_mv.col;
            out[i].ref[k] = m[i].ref_frame[k];
        }
    }
}

static void setup_frame(lv_dec *d, const rl_vp9_frame *f)
{
    VP9Decoder *pbi = d->pbi;
    VP9_COMMON *cm = &pbi->common;
    MACROBLOCKD *xd = &pbi->mb;
    RefCntBuffer *bufs = d->pool.frame_bufs;
    int w = (int)f->width, h = (int)f->height;

    cm->last_frame_type = cm->frame_type;
    cm->last_intra_only = cm->intra_only;
    cm->frame_type = f->key_frame ? KEY_FRAME : INTER_FRAME;
    cm->intra_only = f->intra_only;
    cm->error_resilient_mode = f->error_resilient;
    cm->show_frame = 1;
    cm->profile = PROFILE_0;
    cm->bit_depth = VPX_BITS_8;
    cm->subsampling_x = 1;
    cm->subsampling_y = 1;

    cm->new_fb_idx = BUF_CUR;
    cm->cur_frame = &bufs[BUF_CUR];
    pbi->cur_buf = &bufs[BUF_CUR];
    resize_context_buffers(cm, w, h);
    alloc_buffer(cm, &bufs[BUF_CUR], w, h);
    xd->cur_buf = &bufs[BUF_CUR].buf;

    if (frame_is_intra_only(cm)) {
        for (int i = 0; i < REFS_PER_FRAME; i++) {
            cm->frame_refs[i].idx = INVALID_IDX;
            cm->frame_refs[i].buf = NULL;
        }
    } else {
        for (int i = 0; i < REFS_PER_FRAME; i++) {
            RefCntBuffer *b = &bufs[BUF_REF0 + i];
            load_ref(cm, b, &f->refs[i]);
            if (!valid_ref_frame_size(b->buf.y_crop_width, b->buf.y_crop_height, w, h))
                vpx_internal_error(&cm->error, VPX_CODEC_CORRUPT_FRAME, "Referenced frame has invalid size");
            cm->frame_refs[i].idx = BUF_REF0 + i;
            cm->frame_refs[i].buf = &b->buf;
            vp9_setup_scale_factors_for_frame(&cm->frame_refs[i].sf, b->buf.y_crop_width, b->buf.y_crop_height, w, h);
        }
    }
    for (int i = 0; i < 4; i++)
        cm->ref_frame_sign_bias[i] = f->ref_frame_sign_bias[i];
    cm->allow_high_precision_mv = f->allow_high_precision_mv;
    cm->interp_filter = (INTERP_FILTER)f->interp_filter;

    cm->refresh_frame_context = 0;
    cm->frame_parallel_decoding_mode = 0;
    vp9_zero(cm->counts);

    struct loopfilter *lf = &cm->lf;
    lf->filter_level = f->filter_level;
    lf->sharpness_level = f->sharpness;
    lf->mode_ref_delta_enabled = f->mode_ref_delta_enabled;
    lf->mode_ref_delta_update = 0;
    memcpy(lf->ref_deltas, f->ref_deltas, sizeof lf->ref_deltas);
    memcpy(lf->mode_deltas, f->mode_deltas, sizeof lf->mode_deltas);

    cm->base_qindex = f->base_q_idx;
    cm->y_dc_delta_q = f->delta_q_y_dc;
    cm->uv_dc_delta_q = f->delta_q_uv_dc;
    cm->uv_ac_delta_q = f->delta_q_uv_ac;
    cm->dequant_bit_depth = VPX_BITS_8;
    xd->lossless = f->lossless;

    struct segmentation *seg = &cm->seg;
    seg->enabled = f->seg_enabled;
    seg->update_map = f->seg_update_map;
    seg->temporal_update = f->seg_temporal_update;
    seg->abs_delta = f->seg_abs_delta;
    seg->update_data = 0;
    memcpy(seg->tree_probs, f->probs->seg_tree, sizeof seg->tree_probs);
    memcpy(seg->pred_probs, f->probs->seg_pred, sizeof seg->pred_probs);
    vp9_clearall_segfeatures(seg);
    for (int i = 0; i < MAX_SEGMENTS; i++)
        for (int k = 0; k < SEG_LVL_MAX; k++)
            if (f->seg_feature_mask[i] & (1u << k)) {
                vp9_enable_segfeature(seg, i, (SEG_LVL_FEATURES)k);
                vp9_set_segdata(seg, i, (SEG_LVL_FEATURES)k, f->seg_feature_data[i][k]);
            }
    setup_segmentation_dequant(cm);
    size_t map_n = (size_t)cm->mi_rows * (size_t)cm->mi_cols;
    if (f->prev_seg_map)
        memcpy(cm->last_frame_seg_map, f->prev_seg_map, map_n);
    else
        memset(cm->last_frame_seg_map, 0, map_n);

    cm->log2_tile_cols = f->log2_tile_cols;
    cm->log2_tile_rows = f->log2_tile_rows;
    cm->tx_mode = (TX_MODE)f->tx_mode;
    cm->reference_mode = (REFERENCE_MODE)f->reference_mode;
    cm->comp_fixed_ref = f->comp_fixed_ref;
    cm->comp_var_ref[0] = f->comp_var_ref[0];
    cm->comp_var_ref[1] = f->comp_var_ref[1];

    cm->use_prev_frame_mvs = f->use_prev_mvs && f->prev_mvs;
    if (cm->use_prev_frame_mvs)
        load_prev_mvs(cm, d, f->prev_mvs);

    vp9_setup_block_planes(xd, cm->subsampling_x, cm->subsampling_y);
    load_probs(cm->fc, f->probs);

    if (lf->filter_level && !cm->skip_loop_filter)
        vp9_loop_filter_frame_init(cm, lf->filter_level);

    int tiles = (1 << cm->log2_tile_cols) * (1 << cm->log2_tile_rows);
    if (pbi->tile_worker_data == NULL || tiles != pbi->total_tiles) {
        vpx_free(pbi->tile_worker_data);
        CHECK_MEM_ERROR(&cm->error, pbi->tile_worker_data, vpx_memalign(32, (size_t)tiles * sizeof(*pbi->tile_worker_data)));
        pbi->total_tiles = tiles;
    }
}

static void store_frame(const YV12_BUFFER_CONFIG *y, const rl_vp9_image *o)
{
    int w = y->y_crop_width, h = y->y_crop_height;
    copy_plane(o->y, (int)o->y_stride, y->y_buffer, y->y_stride, w, h);
    copy_plane(o->u, (int)o->uv_stride, y->u_buffer, y->uv_stride, (w + 1) / 2, (h + 1) / 2);
    copy_plane(o->v, (int)o->uv_stride, y->v_buffer, y->uv_stride, (w + 1) / 2, (h + 1) / 2);
}

static bool lv_decode(void *h, const rl_vp9_frame *f)
{
    lv_dec *d = h;
    if (!d || !f->probs || !f->data || f->width == 0 || f->height == 0 || !f->out.y || f->out.width != f->width ||
        f->out.height != f->height)
        return false;
    VP9Decoder *pbi = d->pbi;
    VP9_COMMON *cm = &pbi->common;
    if (setjmp(cm->error.jmp)) {
        cm->error.setjmp = 0;
        pbi->mb.corrupted = 0;
        return false;
    }
    cm->error.setjmp = 1;
    setup_frame(d, f);
    pbi->mb.corrupted = 0;
    decode_tiles(pbi, f->data, f->data + f->size);
    bool ok = !pbi->mb.corrupted;
    if (ok) {
        store_frame(&cm->cur_frame->buf, &f->out);
        if (f->counts)
            store_counts(f->counts, &cm->counts);
        if (f->mvs && !frame_is_intra_only(cm))
            store_mvs(cm, f->mvs);
        if (f->seg_map)
            memcpy(f->seg_map, cm->current_frame_seg_map, (size_t)cm->mi_rows * (size_t)cm->mi_cols);
    }
    cm->last_width = cm->width;
    cm->last_height = cm->height;
    cm->error.setjmp = 0;
    return ok;
}

static const rl_vp9_backend g_lv = {
    .name = "libvpx",
    .open = lv_open,
    .close = lv_close,
    .decode = lv_decode,
};

const rl_vp9_backend *rl_host_vp9_backend(void) { return &g_lv; }
