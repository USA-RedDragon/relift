/**
 * @file pac.c
 * @brief Pointer authentication ciphers and keys (relift/arm64/pac.h).
 *
 * rl_pac_qarma5_reference is ComputePACQARMA of the Arm pseudocode with
 * isqarma3 false and its helpers (PACCellShuffle, PACCellInvShuffle, PACSub,
 * PACInvSub, PACMult, TweakShuffle, TweakInvShuffle, TweakCellRot,
 * TweakCellInvRot), and core/ir/src/pac.rs computes the same. rl_pac_qarma5
 * folds every linear layer into eight byte-indexed tables and applies the
 * S-box a byte at a time; both agree with the QARMA paper's test vector
 * (tests/test_pac.c).
 */
#include <relift/arm64/pac.h>

#include <pthread.h>
#include <stdbool.h>

uint64_t rl_pac_keys[10];
rl_pac_cipher rl_pac_mode = RL_PAC_FAST;

#define CELL(x, i) (((x) >> (4 * (i))) & 0xf)

static const uint8_t SUB[16] = { 0xb, 0x6, 0x8, 0xf, 0xc, 0x0, 0x9, 0xe, 0x3, 0x7, 0x4, 0x5, 0xd, 0x2, 0x1, 0xa };
static const uint8_t INVSUB[16] = { 0x5, 0xe, 0xd, 0x8, 0xa, 0xb, 0x1, 0x9, 0x2, 0x6, 0xf, 0x0, 0x4, 0xc, 0x7, 0x3 };
static const uint8_t SHUF[16] = { 13, 6, 11, 0, 7, 12, 1, 10, 8, 3, 14, 5, 2, 9, 4, 15 };
static const uint8_t INVSHUF[16] = { 3, 6, 12, 9, 14, 11, 1, 4, 8, 13, 7, 2, 5, 0, 10, 15 };
static const uint8_t TSH[16] = { 4, 5, 6, 7, 11, 2, 3, 8, 12, 13, 14, 15, 0, 1, 10, 9 };
static const uint8_t TROT[16] = { 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1 };
static const uint8_t TISH[16] = { 12, 13, 5, 6, 0, 1, 2, 3, 7, 15, 14, 4, 8, 9, 10, 11 };
static const uint8_t TIROT[16] = { 1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1 };
static const uint64_t RC[5] = { 0, UINT64_C(0x13198A2E03707344), UINT64_C(0xA4093822299F31D0),
                                UINT64_C(0x082EFA98EC4E6C89), UINT64_C(0x452821E638D01377) };
static const uint64_t ALPHA = UINT64_C(0xC0AC29B7C97C50DD);

static uint64_t sub(uint64_t x, const uint8_t *t)
{
    uint64_t o = 0;
    for (int i = 0; i < 16; i++)
        o |= (uint64_t)t[CELL(x, i)] << (4 * i);
    return o;
}

static uint64_t shuf(uint64_t x, const uint8_t *p)
{
    uint64_t o = 0;
    for (int i = 0; i < 16; i++)
        o |= CELL(x, p[i]) << (4 * i);
    return o;
}

static unsigned rotcell(unsigned c, int n)
{
    return ((c << n) | (c >> (4 - n))) & 0xf;
}

static uint64_t mult(uint64_t s)
{
    uint64_t o = 0;
    for (int i = 0; i < 4; i++) {
        unsigned c0 = CELL(s, i), c4 = CELL(s, i + 4), c8 = CELL(s, i + 8), c12 = CELL(s, i + 12);
        unsigned t0 = rotcell(c8, 1) ^ rotcell(c4, 2) ^ rotcell(c0, 1);
        unsigned t1 = rotcell(c12, 1) ^ rotcell(c4, 1) ^ rotcell(c0, 2);
        unsigned t2 = rotcell(c12, 2) ^ rotcell(c8, 1) ^ rotcell(c0, 1);
        unsigned t3 = rotcell(c12, 1) ^ rotcell(c8, 2) ^ rotcell(c4, 1);
        o |= (uint64_t)t3 << (4 * i) | (uint64_t)t2 << (4 * (i + 4)) | (uint64_t)t1 << (4 * (i + 8)) |
             (uint64_t)t0 << (4 * (i + 12));
    }
    return o;
}

static unsigned trot(unsigned c)
{
    return (((c ^ (c >> 1)) & 1) << 3) | (((c >> 3) & 1) << 2) | (((c >> 2) & 1) << 1) | ((c >> 1) & 1);
}

static unsigned tirot(unsigned c)
{
    return (((c >> 2) & 1) << 3) | (((c >> 1) & 1) << 2) | ((c & 1) << 1) | ((c ^ (c >> 3)) & 1);
}

static uint64_t tshuf(uint64_t x, const uint8_t *p, const uint8_t *r, unsigned (*rot)(unsigned))
{
    uint64_t o = 0;
    for (int i = 0; i < 16; i++) {
        unsigned c = (unsigned)CELL(x, p[i]);
        o |= (uint64_t)(r[i] ? rot(c) : c) << (4 * i);
    }
    return o;
}

static uint64_t modk(uint64_t key0)
{
    return ((key0 & 1) << 63) | ((key0 >> 2) << 1) | (((key0 >> 63) ^ (key0 >> 1)) & 1);
}

uint64_t rl_pac_qarma5_reference(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1)
{
    uint64_t modk0 = modk(key0);
    uint64_t tw = modifier, w = data ^ key0;
    for (int i = 0; i <= 4; i++) {
        w ^= key1 ^ tw ^ RC[i];
        if (i > 0)
            w = mult(shuf(w, SHUF));
        w = sub(w, SUB);
        tw = tshuf(tw, TSH, TROT, trot);
    }
    w ^= modk0 ^ tw;
    w = sub(mult(shuf(w, SHUF)), SUB);
    w = mult(shuf(w, SHUF)) ^ key1;
    w = sub(shuf(w, INVSHUF), INVSUB);
    w = shuf(mult(w), INVSHUF) ^ key0 ^ tw;
    for (int i = 0; i <= 4; i++) {
        w = sub(w, INVSUB);
        if (i < 4)
            w = shuf(mult(w), INVSHUF);
        tw = tshuf(tw, TISH, TIROT, tirot);
        w ^= RC[4 - i] ^ key1 ^ tw ^ ALPHA;
    }
    return w ^ modk0;
}

typedef struct lin {
    uint64_t t[8][256];
} lin;

static lin L_SM, L_MI, L_I, L_M, L_T, L_TI;
static uint8_t S8[256], IS8[256];
static pthread_once_t g_tables = PTHREAD_ONCE_INIT;

static uint64_t f_sm(uint64_t x) { return mult(shuf(x, SHUF)); }
static uint64_t f_mi(uint64_t x) { return shuf(mult(x), INVSHUF); }
static uint64_t f_i(uint64_t x) { return shuf(x, INVSHUF); }
static uint64_t f_t(uint64_t x) { return tshuf(x, TSH, TROT, trot); }
static uint64_t f_ti(uint64_t x) { return tshuf(x, TISH, TIROT, tirot); }

static void build(lin *l, uint64_t (*f)(uint64_t))
{
    for (int j = 0; j < 8; j++)
        for (int b = 0; b < 256; b++)
            l->t[j][b] = f((uint64_t)b << (8 * j));
}

static void tables(void)
{
    build(&L_SM, f_sm);
    build(&L_MI, f_mi);
    build(&L_I, f_i);
    build(&L_M, mult);
    build(&L_T, f_t);
    build(&L_TI, f_ti);
    for (int b = 0; b < 256; b++) {
        S8[b] = (uint8_t)(SUB[b & 15] | SUB[b >> 4] << 4);
        IS8[b] = (uint8_t)(INVSUB[b & 15] | INVSUB[b >> 4] << 4);
    }
}

static inline uint64_t ap(const lin *l, uint64_t x)
{
    return l->t[0][x & 255] ^ l->t[1][(x >> 8) & 255] ^ l->t[2][(x >> 16) & 255] ^ l->t[3][(x >> 24) & 255] ^
           l->t[4][(x >> 32) & 255] ^ l->t[5][(x >> 40) & 255] ^ l->t[6][(x >> 48) & 255] ^ l->t[7][x >> 56];
}

static inline uint64_t sb(uint64_t x, const uint8_t *t)
{
    uint64_t o = 0;
    for (int j = 0; j < 8; j++)
        o |= (uint64_t)t[(x >> (8 * j)) & 255] << (8 * j);
    return o;
}

uint64_t rl_pac_qarma5(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1)
{
    pthread_once(&g_tables, tables);
    uint64_t modk0 = modk(key0);
    uint64_t tw = modifier, w = data ^ key0;
    for (int i = 0; i <= 4; i++) {
        w ^= key1 ^ tw ^ RC[i];
        if (i > 0)
            w = ap(&L_SM, w);
        w = sb(w, S8);
        tw = ap(&L_T, tw);
    }
    w ^= modk0 ^ tw;
    w = sb(ap(&L_SM, w), S8);
    w = ap(&L_SM, w) ^ key1;
    w = sb(ap(&L_I, w), IS8);
    w = ap(&L_I, ap(&L_M, w)) ^ key0 ^ tw;
    for (int i = 0; i <= 4; i++) {
        w = sb(w, IS8);
        if (i < 4)
            w = ap(&L_MI, w);
        tw = ap(&L_TI, tw);
        w ^= RC[4 - i] ^ key1 ^ tw ^ ALPHA;
    }
    return w ^ modk0;
}

#define MEMO_BITS 8

typedef struct memo_entry {
    uint64_t data, modifier, key0, key1, pac;
} memo_entry;

static _Thread_local memo_entry g_memo[1u << MEMO_BITS];
static _Thread_local bool g_memo_used[1u << MEMO_BITS];

uint64_t rl_pac_qarma5_memo(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1)
{
    unsigned i = (unsigned)(rl_pac_fold(data ^ UINT64_C(0x9e3779b97f4a7c15), modifier | 1) >> (64 - MEMO_BITS));
    memo_entry *e = &g_memo[i];
    if (g_memo_used[i] && e->data == data && e->modifier == modifier && e->key0 == key0 && e->key1 == key1)
        return e->pac;
    uint64_t pac = rl_pac_qarma5(data, modifier, key0, key1);
    *e = (memo_entry){ data, modifier, key0, key1, pac };
    g_memo_used[i] = true;
    return pac;
}

void rl_pac_setup(rl_pac_cipher mode, const uint64_t keys[10])
{
    rl_pac_mode = mode;
    for (int i = 0; i < 10; i++)
        rl_pac_keys[i] = keys[i];
}

void rl_pac_seed(uint64_t seed)
{
    for (int i = 0; i < 10; i++) {
        seed += UINT64_C(0x9e3779b97f4a7c15);
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        rl_pac_keys[i] = z ^ (z >> 31);
    }
}
