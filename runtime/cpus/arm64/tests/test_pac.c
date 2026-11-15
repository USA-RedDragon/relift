/**
 * @file test_pac.c
 * @brief The pointer authentication ciphers: QARMA5 against the QARMA
 * paper's test vector and the IR interpreter's values, the table-driven and
 * memoized paths against the reference on random inputs, and the fast
 * cipher's dependence on every input.
 */
#include "runner.h"

#include <relift/arm64/pac.h>

static uint64_t xorshift(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

TEST(pac_qarma5_paper_vector)
{
    /* QARMA64_5 with sigma2: Avanzi, "The QARMA Block Cipher Family", ToSC 2017(1), appendix A.1. */
    uint64_t p = UINT64_C(0xfb623599da6e8127), t = UINT64_C(0x477d469dec0b8762);
    uint64_t w0 = UINT64_C(0x84be85ce9804e94b), k0 = UINT64_C(0xec2802d4e0a488e9);
    CHECK_EQ(rl_pac_qarma5_reference(p, t, w0, k0), UINT64_C(0xc003b93999b33765));
    CHECK_EQ(rl_pac_qarma5(p, t, w0, k0), UINT64_C(0xc003b93999b33765));
    CHECK_EQ(rl_pac_qarma5_memo(p, t, w0, k0), UINT64_C(0xc003b93999b33765));
}

TEST(pac_qarma5_matches_interpreter)
{
    /* relift_ir::interp::eval_pac on the same inputs. */
    static const uint64_t v[][5] = {
        { 0, 0, 0, 0, UINT64_C(0x76243b953592993d) },
        { UINT64_C(0x0000007100001234), UINT64_C(0x000000717ffff000), UINT64_C(0x0123456789abcdef),
          UINT64_C(0xfedcba9876543210), UINT64_C(0xd58da1b4f1ba5e26) },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        CHECK_EQ(rl_pac_qarma5(v[i][0], v[i][1], v[i][2], v[i][3]), v[i][4]);
}

TEST(pac_qarma5_tables_match_reference)
{
    uint64_t s = UINT64_C(88172645463325252);
    for (int i = 0; i < 200000; i++) {
        uint64_t a = xorshift(&s), b = xorshift(&s), c = xorshift(&s), d = xorshift(&s);
        uint64_t want = rl_pac_qarma5_reference(a, b, c, d);
        CHECK_EQ(rl_pac_qarma5(a, b, c, d), want);
        CHECK_EQ(rl_pac_qarma5_memo(a, b, c, d), want);
        CHECK_EQ(rl_pac_qarma5_memo(a, b, c, d), want);
    }
}

TEST(pac_memo_keeps_every_input_apart)
{
    uint64_t lr = UINT64_C(0x7100004560), sp = UINT64_C(0x717ffff000), k0 = 1, k1 = 2;
    uint64_t base = rl_pac_qarma5_memo(lr, sp, k0, k1);
    CHECK_EQ(base, rl_pac_qarma5(lr, sp, k0, k1));
    CHECK_EQ(rl_pac_qarma5_memo(lr ^ 4, sp, k0, k1), rl_pac_qarma5(lr ^ 4, sp, k0, k1));
    CHECK_EQ(rl_pac_qarma5_memo(lr, sp ^ 16, k0, k1), rl_pac_qarma5(lr, sp ^ 16, k0, k1));
    CHECK_EQ(rl_pac_qarma5_memo(lr, sp, k0 ^ 1, k1), rl_pac_qarma5(lr, sp, k0 ^ 1, k1));
    CHECK_EQ(rl_pac_qarma5_memo(lr, sp, k0, k1 ^ 1), rl_pac_qarma5(lr, sp, k0, k1 ^ 1));
    CHECK_EQ(rl_pac_qarma5_memo(lr, sp, k0, k1), base);
}

TEST(pac_fast_depends_on_every_input_bit)
{
    uint64_t in[4] = { UINT64_C(0x7100004560), UINT64_C(0x717ffff000), UINT64_C(0x0123456789abcdef),
                       UINT64_C(0xfedcba9876543210) };
    uint64_t base = rl_pac_fast(in[0], in[1], in[2], in[3]);
    for (int k = 0; k < 4; k++)
        for (int bit = 0; bit < 64; bit++) {
            uint64_t x[4] = { in[0], in[1], in[2], in[3] };
            x[k] ^= UINT64_C(1) << bit;
            uint64_t d = base ^ rl_pac_fast(x[0], x[1], x[2], x[3]);
            CHECK(__builtin_popcountll(d) >= 12);
        }
}

TEST(pac_dispatch_follows_the_mode)
{
    uint64_t keys[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    rl_pac_setup(RL_PAC_QARMA5, keys);
    CHECK_EQ(rl_pac(11, 12, rl_pac_keys[2], rl_pac_keys[3]), rl_pac_qarma5(11, 12, 3, 4));
    rl_pac_setup(RL_PAC_FAST, keys);
    CHECK_EQ(rl_pac(11, 12, rl_pac_keys[2], rl_pac_keys[3]), rl_pac_fast(11, 12, 3, 4));
    rl_pac_seed(7);
    uint64_t a = rl_pac_keys[0];
    rl_pac_seed(7);
    CHECK_EQ(rl_pac_keys[0], a);
    CHECK(rl_pac_keys[0] != rl_pac_keys[1]);
}
