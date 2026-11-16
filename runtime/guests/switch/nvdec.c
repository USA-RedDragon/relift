/**
 * @file nvdec.c
 * @brief NVDEC, the video decoder engine: VP9 pictures.
 *
 * The method register offsets, the picture setup and probability table
 * layouts, the counter buffer, the motion vector buffer and the surface
 * layout (NV12, block-linear with two-GOB blocks) follow Ryujinx (MIT,
 * Copyright (c) Ryujinx Team and Contributors) Ryujinx.Graphics.Nvdec. The
 * guest parses the frame headers; the picture is decoded by the runtime's
 * VP9 backend (relift/video.h). Reference pictures are read back from guest
 * memory, as the engine reads them.
 */
#include "host1x.h"

#include <relift/video.h>

#include <stdlib.h>
#include <string.h>

enum {
    R_APPLICATION_ID = 0x80,
    R_DRV_PIC_SETUP = 0x101,
    R_IN_BUF_BASE = 0x102,
    R_PICTURE_LUMA = 0x10C,
    R_PICTURE_CHROMA = 0x11D,
    R_VP9_PROB_TAB = 0x170,
    R_VP9_CTX_COUNTER = 0x171,
    R_VP9_SEG_READ = 0x172,
    R_VP9_SEG_WRITE = 0x173,
    R_VP9_COL_MV_WRITE = 0x175,
    R_VP9_COL_MV_READ = 0x176
};

enum { APP_VP9 = 9 };

enum { F_KEY = 1u << 0, F_LAST_KEY = 1u << 1, F_SIZE_CHANGED = 1u << 2, F_ERROR_RES = 1u << 3, F_LAST_SHOW = 1u << 4, F_INTRA_ONLY = 1u << 5 };

#define SURFACE_BLOCK_H_LOG2 1u


typedef struct vp9_size {
    uint16_t width, height, luma_pitch, chroma_pitch;
} vp9_size;

typedef struct vp9_pic {
    uint32_t unknown0[12];
    uint32_t bitstream_size;
    uint32_t is_encrypted;
    uint32_t unknown38;
    uint32_t reserved3c;
    uint32_t block_layout;
    uint32_t work_buffer_size_shr8;
    vp9_size last, golden, alt, cur;
    uint32_t flags;
    int8_t ref_frame_sign_bias[4];
    uint8_t first_level, sharpness, base_q_index;
    int8_t y_dc_delta_q, uv_ac_delta_q, uv_dc_delta_q;
    uint8_t lossless, tx_mode, allow_high_precision_mv, interp_filter, reference_mode;
    int8_t comp_fixed_ref;
    int8_t comp_var_ref[2];
    uint8_t log2_tile_cols, log2_tile_rows;
    uint8_t seg_enabled, seg_update_map, seg_temporal_update, seg_abs_delta;
    uint32_t seg_feature_mask[8];
    int16_t seg_feature_data[8][4];
    uint8_t mode_ref_delta_enabled;
    int8_t ref_deltas[4];
    int8_t mode_deltas[2];
    uint8_t padding_eb;
    uint32_t work_buffer_size_shr8_new;
    uint32_t surface_params;
    uint32_t unknown_f4[3];
} vp9_pic;

typedef struct vp9_hw_probs {
    uint8_t kf_y_mode_e0_e7[10][10][8];
    uint8_t kf_y_mode_e8[10][10];
    uint8_t padding384[3];
    uint8_t seg_tree[7];
    uint8_t seg_pred[3];
    uint8_t padding391[15];
    uint8_t kf_uv_mode_e0_e7[10][8];
    uint8_t kf_uv_mode_e8[10];
    uint8_t padding3fa[6];
    uint8_t inter_mode[7][4];
    uint8_t intra_inter[4];
    uint8_t uv_mode_e0_e7[10][8];
    uint8_t tx_8x8[2][1];
    uint8_t tx_16x16[2][2];
    uint8_t tx_32x32[2][3];
    uint8_t y_mode_e8[4];
    uint8_t y_mode_e0_e7[4][8];
    uint8_t kf_partition[16][4];
    uint8_t partition[16][4];
    uint8_t uv_mode_e8[10];
    uint8_t switchable_interp[4][2];
    uint8_t comp_inter[5];
    uint8_t skip[4];
    uint8_t mv_joints[3];
    uint8_t mv_sign[2];
    uint8_t mv_class0[2][1];
    uint8_t mv_fp[2][3];
    uint8_t mv_class0_hp[2];
    uint8_t mv_hp[2];
    uint8_t mv_classes[2][10];
    uint8_t mv_class0_fp[2][2][3];
    uint8_t mv_bits[2][10];
    uint8_t single_ref[5][2];
    uint8_t comp_ref[5];
    uint8_t padding58f[17];
    uint8_t coef[4][2][2][6][6][4];
} vp9_hw_probs;

typedef struct vp9_hw_counts {
    uint32_t inter_mode[7][3][2];
    uint32_t y_mode[4][10];
    uint32_t uv_mode[10][10];
    uint32_t partition[16][4];
    uint32_t switchable_interp[4][3];
    uint32_t intra_inter[4][2];
    uint32_t comp_inter[5][2];
    uint32_t single_ref[5][2][2];
    uint32_t comp_ref[5][2];
    uint32_t tx_32x32[2][4];
    uint32_t tx_16x16[2][3];
    uint32_t tx_8x8[2][2];
    uint32_t skip[3][2];
    uint32_t mv_joints[4];
    uint32_t mv_sign[2][2];
    uint32_t mv_classes[2][11];
    uint32_t mv_class0[2][2];
    uint32_t mv_bits[2][10][2];
    uint32_t mv_class0_fp[2][2][4];
    uint32_t mv_fp[2][4];
    uint32_t mv_class0_hp[2][2];
    uint32_t mv_hp[2][2];
    uint32_t coef[4][2][2][6][6][4];
    uint32_t eob_branch[4][2][2][6][6];
} vp9_hw_counts;

typedef struct vp9_hw_mvref {
    int16_t mv[2][2];
    int32_t ref[2];
} vp9_hw_mvref;


_Static_assert(sizeof(vp9_pic) == 0x100 && offsetof(vp9_pic, seg_feature_mask) == 0x84 && offsetof(vp9_pic, surface_params) == 0xF0, "vp9 picture setup");
_Static_assert(sizeof(vp9_hw_probs) == 0xEA0, "vp9 probability table");
_Static_assert(offsetof(vp9_hw_probs, coef) == 0x5A0, "vp9 coefficient probabilities");
_Static_assert(sizeof(vp9_hw_mvref) == 16, "vp9 motion vector");

typedef struct nvdec_state {
    const rl_vp9_backend *vp9;
    void *dec;
    uint8_t *planes;
    size_t planes_size;
    rl_vp9_mvref *mvs_in, *mvs_out;
    size_t mvs_n;
} nvdec_state;

static nvdec_state st;

static uint8_t *dev_ptr(uint32_t reg, uint64_t size) { return reg ? h1x_ptr((uint64_t)reg << 8, size) : NULL; }

static void convert_probs(rl_vp9_probs *o, const vp9_hw_probs *p)
{
    for (int i = 0; i < 4; i++) {
        memcpy(o->y_mode[i], p->y_mode_e0_e7[i], 8);
        o->y_mode[i][8] = p->y_mode_e8[i];
    }
    for (int i = 0; i < 10; i++) {
        memcpy(o->uv_mode[i], p->uv_mode_e0_e7[i], 8);
        o->uv_mode[i][8] = p->uv_mode_e8[i];
    }
    for (int i = 0; i < 16; i++)
        memcpy(o->partition[i], p->partition[i], 3);
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < 6; l++)
                        memcpy(o->coef[t][i][j][k][l], p->coef[t][i][j][k][l], 3);
    memcpy(o->switchable_interp, p->switchable_interp, sizeof o->switchable_interp);
    for (int i = 0; i < 7; i++)
        memcpy(o->inter_mode[i], p->inter_mode[i], 3);
    memcpy(o->intra_inter, p->intra_inter, sizeof o->intra_inter);
    memcpy(o->comp_inter, p->comp_inter, sizeof o->comp_inter);
    memcpy(o->single_ref, p->single_ref, sizeof o->single_ref);
    memcpy(o->comp_ref, p->comp_ref, sizeof o->comp_ref);
    memcpy(o->tx_32x32, p->tx_32x32, sizeof o->tx_32x32);
    memcpy(o->tx_16x16, p->tx_16x16, sizeof o->tx_16x16);
    memcpy(o->tx_8x8, p->tx_8x8, sizeof o->tx_8x8);
    memcpy(o->skip, p->skip, sizeof o->skip);
    memcpy(o->mv_joints, p->mv_joints, sizeof o->mv_joints);
    memcpy(o->mv_sign, p->mv_sign, sizeof o->mv_sign);
    memcpy(o->mv_classes, p->mv_classes, sizeof o->mv_classes);
    memcpy(o->mv_class0, p->mv_class0, sizeof o->mv_class0);
    memcpy(o->mv_bits, p->mv_bits, sizeof o->mv_bits);
    memcpy(o->mv_class0_fp, p->mv_class0_fp, sizeof o->mv_class0_fp);
    memcpy(o->mv_fp, p->mv_fp, sizeof o->mv_fp);
    memcpy(o->mv_class0_hp, p->mv_class0_hp, sizeof o->mv_class0_hp);
    memcpy(o->mv_hp, p->mv_hp, sizeof o->mv_hp);
    memcpy(o->seg_tree, p->seg_tree, sizeof o->seg_tree);
    memcpy(o->seg_pred, p->seg_pred, sizeof o->seg_pred);
}

static void convert_counts(vp9_hw_counts *o, const rl_vp9_counts *c)
{
    for (int i = 0; i < 7; i++) {
        const uint32_t *m = c->inter_mode[i];
        o->inter_mode[i][0][0] = m[2];
        o->inter_mode[i][0][1] = m[0] + m[1] + m[3];
        o->inter_mode[i][1][0] = m[0];
        o->inter_mode[i][1][1] = m[1] + m[3];
        o->inter_mode[i][2][0] = m[1];
        o->inter_mode[i][2][1] = m[3];
    }
    memcpy(o->y_mode, c->y_mode, sizeof o->y_mode);
    memcpy(o->uv_mode, c->uv_mode, sizeof o->uv_mode);
    memcpy(o->partition, c->partition, sizeof o->partition);
    memcpy(o->switchable_interp, c->switchable_interp, sizeof o->switchable_interp);
    memcpy(o->intra_inter, c->intra_inter, sizeof o->intra_inter);
    memcpy(o->comp_inter, c->comp_inter, sizeof o->comp_inter);
    memcpy(o->single_ref, c->single_ref, sizeof o->single_ref);
    memcpy(o->comp_ref, c->comp_ref, sizeof o->comp_ref);
    memcpy(o->tx_32x32, c->tx_32x32, sizeof o->tx_32x32);
    memcpy(o->tx_16x16, c->tx_16x16, sizeof o->tx_16x16);
    memcpy(o->tx_8x8, c->tx_8x8, sizeof o->tx_8x8);
    memcpy(o->skip, c->skip, sizeof o->skip);
    memcpy(o->mv_joints, c->mv_joints, sizeof o->mv_joints);
    memcpy(o->mv_sign, c->mv_sign, sizeof o->mv_sign);
    memcpy(o->mv_classes, c->mv_classes, sizeof o->mv_classes);
    memcpy(o->mv_class0, c->mv_class0, sizeof o->mv_class0);
    memcpy(o->mv_bits, c->mv_bits, sizeof o->mv_bits);
    memcpy(o->mv_class0_fp, c->mv_class0_fp, sizeof o->mv_class0_fp);
    memcpy(o->mv_fp, c->mv_fp, sizeof o->mv_fp);
    memcpy(o->mv_class0_hp, c->mv_class0_hp, sizeof o->mv_class0_hp);
    memcpy(o->mv_hp, c->mv_hp, sizeof o->mv_hp);
    memcpy(o->coef, c->coef, sizeof o->coef);
    memcpy(o->eob_branch, c->eob_branch, sizeof o->eob_branch);
}

static uint32_t chroma_w(uint32_t w) { return (w + 1) / 2; }

/** @brief The guest surface of a picture slot: NV12 in two block-linear planes. */
static bool surface_of(const uint32_t *regs, uint32_t slot, const vp9_size *size, h1x_bl *luma, h1x_bl *chroma)
{
    uint32_t w = size->width, h = size->height;
    if (!w || !h)
        return false;
    *luma = (h1x_bl){ NULL, w, h, SURFACE_BLOCK_H_LOG2 };
    *chroma = (h1x_bl){ NULL, chroma_w(w) * 2, (h + 1) / 2, SURFACE_BLOCK_H_LOG2 };
    luma->base = dev_ptr(regs[R_PICTURE_LUMA + slot], h1x_bl_size(luma->width, luma->height, SURFACE_BLOCK_H_LOG2));
    chroma->base = dev_ptr(regs[R_PICTURE_CHROMA + slot], h1x_bl_size(chroma->width, chroma->height, SURFACE_BLOCK_H_LOG2));
    return luma->base && chroma->base;
}

static void image_alloc(rl_vp9_image *im, uint8_t **mem, uint32_t w, uint32_t h)
{
    uint32_t cw = chroma_w(w), ch = (h + 1) / 2;
    *im = (rl_vp9_image){ w, h, *mem, *mem + (size_t)w * h, *mem + (size_t)w * h + (size_t)cw * ch, w, cw };
    *mem += (size_t)w * h + 2 * (size_t)cw * ch;
}

static void read_surface(rl_vp9_image *im, const h1x_bl *luma, const h1x_bl *chroma, uint8_t *row)
{
    uint32_t cw = chroma_w(im->width);
    for (uint32_t y = 0; y < im->height; y++)
        h1x_bl_read_row(luma, 0, y, im->y + (size_t)y * im->y_stride, im->width);
    for (uint32_t y = 0; y < chroma->height; y++) {
        h1x_bl_read_row(chroma, 0, y, row, cw * 2);
        for (uint32_t x = 0; x < cw; x++) {
            im->u[(size_t)y * im->uv_stride + x] = row[2 * x];
            im->v[(size_t)y * im->uv_stride + x] = row[2 * x + 1];
        }
    }
}

static void write_surface(const rl_vp9_image *im, const h1x_bl *luma, const h1x_bl *chroma, uint8_t *row)
{
    uint32_t cw = chroma_w(im->width);
    for (uint32_t y = 0; y < im->height; y++)
        h1x_bl_write_row(luma, 0, y, im->y + (size_t)y * im->y_stride, im->width);
    for (uint32_t y = 0; y < chroma->height; y++) {
        for (uint32_t x = 0; x < cw; x++) {
            row[2 * x] = im->u[(size_t)y * im->uv_stride + x];
            row[2 * x + 1] = im->v[(size_t)y * im->uv_stride + x];
        }
        h1x_bl_write_row(chroma, 0, y, row, cw * 2);
    }
}

static size_t image_bytes(const vp9_size *s) { return (size_t)s->width * s->height + 2 * (size_t)chroma_w(s->width) * ((s->height + 1u) / 2); }

static bool grow(void **p, size_t *cap, size_t n)
{
    if (n <= *cap)
        return true;
    void *q = realloc(*p, n);
    if (!q)
        return false;
    *p = q;
    *cap = n;
    return true;
}

static void decode_vp9(const uint32_t *regs)
{
    const vp9_pic *pic = (const vp9_pic *)(void *)dev_ptr(regs[R_DRV_PIC_SETUP], sizeof(vp9_pic));
    const vp9_hw_probs *hp = (const vp9_hw_probs *)(void *)dev_ptr(regs[R_VP9_PROB_TAB], sizeof(vp9_hw_probs));
    vp9_hw_counts *hc = (vp9_hw_counts *)(void *)dev_ptr(regs[R_VP9_CTX_COUNTER], sizeof(vp9_hw_counts));
    if (!pic || !hp) {
        rl_log(RL_LOG_WARN, "nvdec", "vp9: picture setup or probability table not mapped");
        return;
    }
    const uint8_t *data = dev_ptr(regs[R_IN_BUF_BASE], pic->bitstream_size);
    if (!data) {
        rl_log(RL_LOG_WARN, "nvdec", "vp9: bitstream not mapped");
        return;
    }
    if (!st.vp9) {
        st.vp9 = rl_vp9_backend_current();
        st.dec = st.vp9->open();
        if (!st.dec)
            rl_log(RL_LOG_WARN, "nvdec", "vp9: no decoder in this build (%s); pictures stay undecoded", st.vp9->name);
    }
    if (!st.dec)
        return;

    uint32_t flags = pic->flags;
    bool key = flags & F_KEY, intra_only = flags & F_INTRA_ONLY;
    const vp9_size *sizes[4] = { &pic->last, &pic->golden, &pic->alt, &pic->cur };
    h1x_bl luma[4], chroma[4];
    if (!surface_of(regs, 3, &pic->cur, &luma[3], &chroma[3])) {
        rl_log(RL_LOG_WARN, "nvdec", "vp9: output surface not mapped");
        return;
    }
    size_t need = image_bytes(&pic->cur) + 2 * (size_t)pic->cur.width + 128;
    if (!key && !intra_only)
        for (int i = 0; i < 3; i++) {
            if (!surface_of(regs, (uint32_t)i, sizes[i], &luma[i], &chroma[i])) {
                rl_log(RL_LOG_WARN, "nvdec", "vp9: reference %d not mapped", i);
                return;
            }
            need += image_bytes(sizes[i]);
        }
    if (!grow((void **)&st.planes, &st.planes_size, need))
        return;

    rl_vp9_probs probs;
    convert_probs(&probs, hp);
    static const uint8_t literal_to_filter[4] = { RL_VP9_EIGHTTAP_SMOOTH, RL_VP9_EIGHTTAP, RL_VP9_EIGHTTAP_SHARP, RL_VP9_BILINEAR };
    rl_vp9_frame f = {
        .width = pic->cur.width,
        .height = pic->cur.height,
        .key_frame = key,
        .intra_only = intra_only,
        .error_resilient = flags & F_ERROR_RES,
        .allow_high_precision_mv = pic->allow_high_precision_mv,
        .interp_filter = pic->interp_filter < 4 ? literal_to_filter[pic->interp_filter] : RL_VP9_SWITCHABLE,
        .reference_mode = pic->reference_mode,
        .comp_fixed_ref = pic->comp_fixed_ref,
        .comp_var_ref = { pic->comp_var_ref[0], pic->comp_var_ref[1] },
        .base_q_idx = pic->base_q_index,
        .delta_q_y_dc = pic->y_dc_delta_q,
        .delta_q_uv_dc = pic->uv_dc_delta_q,
        .delta_q_uv_ac = pic->uv_ac_delta_q,
        .lossless = pic->lossless,
        .tx_mode = pic->tx_mode,
        .log2_tile_cols = pic->log2_tile_cols,
        .log2_tile_rows = pic->log2_tile_rows,
        .filter_level = pic->first_level,
        .sharpness = pic->sharpness,
        .mode_ref_delta_enabled = pic->mode_ref_delta_enabled,
        .seg_enabled = pic->seg_enabled,
        .seg_update_map = pic->seg_update_map,
        .seg_temporal_update = pic->seg_temporal_update,
        .seg_abs_delta = pic->seg_abs_delta,
        .use_prev_mvs = !(flags & (F_ERROR_RES | F_SIZE_CHANGED | F_INTRA_ONLY | F_LAST_KEY)) && (flags & F_LAST_SHOW),
        .probs = &probs,
        .data = data,
        .size = pic->bitstream_size,
    };
    memcpy(f.ref_frame_sign_bias, pic->ref_frame_sign_bias, 4);
    memcpy(f.ref_deltas, pic->ref_deltas, 4);
    memcpy(f.mode_deltas, pic->mode_deltas, 2);
    memcpy(f.seg_feature_mask, pic->seg_feature_mask, sizeof f.seg_feature_mask);
    memcpy(f.seg_feature_data, pic->seg_feature_data, sizeof f.seg_feature_data);

    uint8_t *mem = st.planes;
    image_alloc(&f.out, &mem, f.width, f.height);
    if (!key && !intra_only)
        for (int i = 0; i < 3; i++) {
            image_alloc(&f.refs[i], &mem, sizes[i]->width, sizes[i]->height);
            read_surface(&f.refs[i], &luma[i], &chroma[i], mem);
        }
    uint8_t *row = mem;

    size_t mi_n = (size_t)((f.width + 7) / 8) * ((f.height + 7) / 8);
    if (mi_n > st.mvs_n) {
        rl_vp9_mvref *a = realloc(st.mvs_in, mi_n * sizeof *a), *b = a ? realloc(st.mvs_out, mi_n * sizeof *b) : NULL;
        if (a)
            st.mvs_in = a;
        if (b)
            st.mvs_out = b;
        if (!a || !b)
            return;
        st.mvs_n = mi_n;
    }
    vp9_hw_mvref *mv_read = (vp9_hw_mvref *)(void *)dev_ptr(regs[R_VP9_COL_MV_READ], mi_n * sizeof(vp9_hw_mvref));
    vp9_hw_mvref *mv_write = (vp9_hw_mvref *)(void *)dev_ptr(regs[R_VP9_COL_MV_WRITE], mi_n * sizeof(vp9_hw_mvref));
    if (f.use_prev_mvs && mv_read) {
        for (size_t i = 0; i < mi_n; i++) {
            memcpy(st.mvs_in[i].mv, mv_read[i].mv, sizeof st.mvs_in[i].mv);
            st.mvs_in[i].ref[0] = (int8_t)mv_read[i].ref[0];
            st.mvs_in[i].ref[1] = (int8_t)mv_read[i].ref[1];
        }
        f.prev_mvs = st.mvs_in;
    }
    f.mvs = mv_write ? st.mvs_out : NULL;
    f.prev_seg_map = dev_ptr(regs[R_VP9_SEG_READ], mi_n);
    f.seg_map = dev_ptr(regs[R_VP9_SEG_WRITE], mi_n);
    if (!f.seg_enabled)
        f.prev_seg_map = NULL;

    rl_log(RL_LOG_DEBUG, "nvdec", "vp9 %ux%u flags 0x%x size %u q %u lf %u tiles %u/%u", f.width, f.height, flags, pic->bitstream_size, pic->base_q_index,
           pic->first_level, pic->log2_tile_cols, pic->log2_tile_rows);
    rl_vp9_counts counts;
    memset(&counts, 0, sizeof counts);
    f.counts = &counts;
    if (!st.vp9->decode(st.dec, &f)) {
        rl_log(RL_LOG_WARN, "nvdec", "vp9: frame %ux%u did not decode", f.width, f.height);
        return;
    }
    write_surface(&f.out, &luma[3], &chroma[3], row);
    if (hc)
        convert_counts(hc, &counts);
    if (mv_write && !key && !intra_only) {
        for (size_t i = 0; i < mi_n; i++) {
            memcpy(mv_write[i].mv, st.mvs_out[i].mv, sizeof mv_write[i].mv);
            mv_write[i].ref[0] = st.mvs_out[i].ref[0];
            mv_write[i].ref[1] = st.mvs_out[i].ref[1];
        }
    }
}

void nvdec_execute(const uint32_t *regs)
{
    switch (regs[R_APPLICATION_ID]) {
    case APP_VP9:
        decode_vp9(regs);
        break;
    default:
        rl_log(RL_LOG_WARN, "nvdec", "codec %u is not supported", regs[R_APPLICATION_ID]);
        break;
    }
}

void nvdec_reset(void)
{
    if (st.vp9 && st.dec)
        st.vp9->close(st.dec);
    free(st.planes);
    free(st.mvs_in);
    free(st.mvs_out);
    memset(&st, 0, sizeof st);
}
