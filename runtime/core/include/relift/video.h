/**
 * @file video.h
 * @brief Video decoding for hardware decoders that a guest drives below the
 * bitstream headers.
 *
 * VP9: the guest parses the uncompressed and compressed frame headers itself
 * and hands over the frame parameters, the probability tables to decode with
 * and the tile data; the decoder reconstructs the frame (with the loop
 * filter), and returns the symbol counts the guest adapts its probabilities
 * with and the motion vectors the next frame may use. Every array has the
 * shape and order of the VP9 specification (the same as libvpx's
 * FRAME_CONTEXT and FRAME_COUNTS). The backend is the host's VP9 decoder
 * when the runtime is built with one (RL_HOST_VP9), else a stub that
 * decodes nothing.
 */
#ifndef RELIFT_VIDEO_H
#define RELIFT_VIDEO_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The adaptive VP9 probability tables, in the order of the specification (key frames use the fixed tables of the specification). */
typedef struct rl_vp9_probs {
    uint8_t y_mode[4][9];
    uint8_t uv_mode[10][9];
    uint8_t partition[16][3];
    /** @brief [tx size][plane type][ref][band][context][node]. */
    uint8_t coef[4][2][2][6][6][3];
    uint8_t switchable_interp[4][2];
    uint8_t inter_mode[7][3];
    uint8_t intra_inter[4];
    uint8_t comp_inter[5];
    uint8_t single_ref[5][2];
    uint8_t comp_ref[5];
    uint8_t tx_32x32[2][3];
    uint8_t tx_16x16[2][2];
    uint8_t tx_8x8[2][1];
    uint8_t skip[3];
    uint8_t mv_joints[3];
    uint8_t mv_sign[2];
    uint8_t mv_classes[2][10];
    uint8_t mv_class0[2][1];
    uint8_t mv_bits[2][10];
    uint8_t mv_class0_fp[2][2][3];
    uint8_t mv_fp[2][3];
    uint8_t mv_class0_hp[2];
    uint8_t mv_hp[2];
    uint8_t seg_tree[7];
    uint8_t seg_pred[3];
} rl_vp9_probs;

/** @brief VP9 symbol counts of one frame, in the order of the specification. */
typedef struct rl_vp9_counts {
    uint32_t y_mode[4][10];
    uint32_t uv_mode[10][10];
    uint32_t partition[16][4];
    /** @brief [tx size][plane type][ref][band][context][token class: zero, one, more, EOB model]. */
    uint32_t coef[4][2][2][6][6][4];
    uint32_t eob_branch[4][2][2][6][6];
    uint32_t switchable_interp[4][3];
    /** @brief [context][mode: NEARESTMV, NEARMV, ZEROMV, NEWMV]. */
    uint32_t inter_mode[7][4];
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
} rl_vp9_counts;

/** @brief An 8-bit 4:2:0 picture in three planes. */
typedef struct rl_vp9_image {
    uint32_t width, height;
    uint8_t *y, *u, *v;
    uint32_t y_stride, uv_stride;
} rl_vp9_image;

/** @brief The motion vectors of one 8x8 block, kept for the next frame's candidate list. */
typedef struct rl_vp9_mvref {
    /** @brief [which][row, column] in 1/8 pixels. */
    int16_t mv[2][2];
    /** @brief Reference frames (0 intra, 1 last, 2 golden, 3 altref; -1 none). */
    int8_t ref[2];
} rl_vp9_mvref;

/** @brief Interpolation filters (frame interp_filter). */
enum { RL_VP9_EIGHTTAP = 0, RL_VP9_EIGHTTAP_SMOOTH = 1, RL_VP9_EIGHTTAP_SHARP = 2, RL_VP9_BILINEAR = 3, RL_VP9_SWITCHABLE = 4 };

/** @brief One frame to decode: the header values the guest parsed and the buffers. */
typedef struct rl_vp9_frame {
    uint32_t width, height;
    bool key_frame, intra_only, error_resilient;
    bool allow_high_precision_mv;
    /** @brief RL_VP9_*. */
    uint8_t interp_filter;
    int8_t ref_frame_sign_bias[4];
    /** @brief 0 single, 1 compound, 2 per block. */
    uint8_t reference_mode;
    int8_t comp_fixed_ref;
    int8_t comp_var_ref[2];
    uint8_t base_q_idx;
    int8_t delta_q_y_dc, delta_q_uv_dc, delta_q_uv_ac;
    bool lossless;
    /** @brief 0 only 4x4 to 3 allow 32x32, 4 per block. */
    uint8_t tx_mode;
    uint8_t log2_tile_cols, log2_tile_rows;
    uint8_t filter_level, sharpness;
    bool mode_ref_delta_enabled;
    int8_t ref_deltas[4];
    int8_t mode_deltas[2];
    bool seg_enabled, seg_update_map, seg_temporal_update, seg_abs_delta;
    uint32_t seg_feature_mask[8];
    int16_t seg_feature_data[8][4];
    /** @brief The previous frame's motion vectors take part in the candidate lists. */
    bool use_prev_mvs;
    const rl_vp9_probs *probs;
    /** @brief Filled with the frame's counts. */
    rl_vp9_counts *counts;
    /** @brief Tile data: the tile size markers and tiles after the compressed header. */
    const uint8_t *data;
    size_t size;
    /** @brief Last, golden, altref; unused for intra frames. */
    rl_vp9_image refs[3];
    /** @brief Receives the reconstructed frame (width x height). */
    rl_vp9_image out;
    /** @brief The previous frame's motion vectors (mi rows x mi columns), when use_prev_mvs. */
    const rl_vp9_mvref *prev_mvs;
    /** @brief Receives this frame's motion vectors (mi rows x mi columns). */
    rl_vp9_mvref *mvs;
    /** @brief The previous segment map (one id per 8x8 block), NULL for all zero. */
    const uint8_t *prev_seg_map;
    /** @brief Receives this frame's segment map, may be NULL. */
    uint8_t *seg_map;
} rl_vp9_frame;

/** @brief A VP9 decoder backend. */
typedef struct rl_vp9_backend {
    /** @brief Short name for logs. */
    const char *name;
    /** @brief A decoder instance, NULL when there is none. */
    void *(*open)(void);
    void (*close)(void *d);
    /** @brief Decodes one frame; false when it cannot (corrupt data, no decoder). */
    bool (*decode)(void *d, const rl_vp9_frame *f);
} rl_vp9_backend;

/**
 * @brief A VP9 decoder that takes whole compressed frames, headers included (a service that receives the stream
 * rather than a hardware decoder's tile data).
 */
typedef struct rl_vp9_stream_backend {
    /** @brief Short name for logs. */
    const char *name;
    /** @brief A decoder instance, NULL when there is none. */
    void *(*open)(void);
    void (*close)(void *d);
    /**
     * @brief Decodes one compressed frame or superframe. Returns 1 when a frame is to be shown (out then points
     * at the decoder's own planes, valid until the next call), 0 when the data held none, -1 when it is corrupt
     * or there is no decoder.
     */
    int (*decode)(void *d, const uint8_t *data, size_t size, rl_vp9_image *out);
} rl_vp9_stream_backend;

/** @brief The picture every whole-frame backend returns: 8-bit 4:2:0 in three planes. */
typedef rl_vp9_image rl_video_image;

/**
 * @brief An AV1 decoder that takes the stream's temporal units (OBUs). Pictures of other bit depths or chroma
 * layouts come back converted to 8-bit 4:2:0.
 */
typedef struct rl_av1_stream_backend {
    /** @brief Short name for logs. */
    const char *name;
    /** @brief A decoder instance, NULL when there is none. */
    void *(*open)(void);
    void (*close)(void *d);
    /** @brief As rl_vp9_stream_backend::decode, for one temporal unit. */
    int (*decode)(void *d, const uint8_t *data, size_t size, rl_video_image *out);
} rl_av1_stream_backend;

/** @brief The AV1 backend: the host's when built in, else a stub that decodes nothing. */
RL_RUNTIME_API const rl_av1_stream_backend *rl_av1_stream_backend_current(void);
/** @brief The host's AV1 backend (runtime/host, only with RL_HOST_AV1). */
RL_RUNTIME_API const rl_av1_stream_backend *rl_host_av1_stream_backend(void);

/** @brief The whole-frame VP9 backend: the host's when built in, else a stub that decodes nothing. */
RL_RUNTIME_API const rl_vp9_stream_backend *rl_vp9_stream_backend_current(void);
/** @brief The host's whole-frame VP9 backend (runtime/host, only with RL_HOST_VP9). */
RL_RUNTIME_API const rl_vp9_stream_backend *rl_host_vp9_stream_backend(void);

/** @brief The VP9 backend: the host's when built in, else the stub. */
RL_RUNTIME_API const rl_vp9_backend *rl_vp9_backend_current(void);
/** @brief The host's VP9 backend (runtime/host, only with RL_HOST_VP9). */
RL_RUNTIME_API const rl_vp9_backend *rl_host_vp9_backend(void);

#endif
