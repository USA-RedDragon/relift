/**
 * @file scene.h
 * @brief Op streams of the display tests: register and memory writes, reads
 * whose values are kept, and waits for a line. The same words run as the
 * ARM9 program of a test ROM on the oracle and through ndst_scene here, so
 * the expected values in the tests are the oracle's.
 */
#ifndef RELIFT_NDS_SCENE_H
#define RELIFT_NDS_SCENE_H

#include <stdint.h>

/** @brief The ops; each is followed by its words. */
enum {
    OP_END = 0,
    OP_W32 = 1,
    OP_W16 = 2,
    OP_FILL32 = 3,
    OP_COPY32 = 4,
    OP_W8 = 5,
    OP_FILL16 = 6,
    OP_RAMP16 = 7,
    OP_WAITLINE = 8,
    OP_R32 = 9,
    OP_R16 = 10,
    OP_R8 = 11,
};

#define W32(a, v) OP_W32, (uint32_t)(a), (uint32_t)(v)
#define W16(a, v) OP_W16, (uint32_t)(a), (uint32_t)(v)
#define W8(a, v) OP_W8, (uint32_t)(a), (uint32_t)(v)
#define FILL32(a, n, v) OP_FILL32, (uint32_t)(a), (uint32_t)(n), (uint32_t)(v)
#define FILL16(a, n, v) OP_FILL16, (uint32_t)(a), (uint32_t)(n), (uint32_t)(v)
#define RAMP16(a, n, v, d) OP_RAMP16, (uint32_t)(a), (uint32_t)(n), (uint32_t)(v), (uint32_t)(d)
#define COPY32(a, n) OP_COPY32, (uint32_t)(a), (uint32_t)(n)
#define WAITLINE(n) OP_WAITLINE, (uint32_t)(n)
#define RD32(a) OP_R32, (uint32_t)(a)
#define RD16(a) OP_R16, (uint32_t)(a)
#define RD8(a) OP_R8, (uint32_t)(a)

#define RGB15(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 5 | (uint32_t)(b) << 10)

#define DISPCNT 0x04000000u
#define DISPSTAT 0x04000004u
#define VCOUNT 0x04000006u
#define BG0CNT 0x04000008u
#define BG1CNT 0x0400000Au
#define BG2CNT 0x0400000Cu
#define BG3CNT 0x0400000Eu
#define BG0HOFS 0x04000010u
#define BG0VOFS 0x04000012u
#define BG1HOFS 0x04000014u
#define BG1VOFS 0x04000016u
#define BG2HOFS 0x04000018u
#define BG2VOFS 0x0400001Au
#define BG3HOFS 0x0400001Cu
#define BG3VOFS 0x0400001Eu
#define BG2PA 0x04000020u
#define BG2PB 0x04000022u
#define BG2PC 0x04000024u
#define BG2PD 0x04000026u
#define BG2X 0x04000028u
#define BG2Y 0x0400002Cu
#define BG3PA 0x04000030u
#define BG3PB 0x04000032u
#define BG3PC 0x04000034u
#define BG3PD 0x04000036u
#define BG3X 0x04000038u
#define BG3Y 0x0400003Cu
#define WIN0H 0x04000040u
#define WIN1H 0x04000042u
#define WIN0V 0x04000044u
#define WIN1V 0x04000046u
#define WININ 0x04000048u
#define WINOUT 0x0400004Au
#define MOSAIC 0x0400004Cu
#define BLDCNT 0x04000050u
#define BLDALPHA 0x04000052u
#define BLDY 0x04000054u
#define DISP3DCNT 0x04000060u
#define DISPCAPCNT 0x04000064u
#define MMEMFIFO 0x04000068u
#define MASTER_BRIGHT 0x0400006Cu
#define POWCNT1 0x04000304u
#define VRAMCNT_A 0x04000240u
#define VRAMCNT_B 0x04000241u
#define VRAMCNT_C 0x04000242u
#define VRAMCNT_D 0x04000243u
#define VRAMCNT_E 0x04000244u
#define VRAMCNT_F 0x04000245u
#define VRAMCNT_G 0x04000246u
#define VRAMCNT_H 0x04000248u
#define VRAMCNT_I 0x04000249u
/** @brief Engine B's registers sit 0x1000 above engine A's. */
#define EB 0x1000u
#define PAL_A_BG 0x05000000u
#define PAL_A_OBJ 0x05000200u
#define PAL_B_BG 0x05000400u
#define PAL_B_OBJ 0x05000600u
#define OAM_A 0x07000000u
#define OAM_B 0x07000400u

#endif
