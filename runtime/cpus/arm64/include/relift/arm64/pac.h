/**
 * @file relift/arm64/pac.h
 * @brief Pointer authentication (FEAT_PAuth): the keys and the cipher of the
 * IR operation pac (Arm ComputePAC), which every PAC, AUT, RETA, BRA, BLRA,
 * LDRA and PACGA instruction calls (docs/abi.md section 10.3).
 *
 * The keys are the HLE kernel's: the guest can neither read nor write them,
 * so the runtime chooses them and the cipher. RL_PAC_QARMA5 is the
 * architected QARMA5 the lifter is checked against; RL_PAC_FAST is a keyed
 * multiply-mix, the default. Every value a guest sees through these
 * instructions depends on keys it cannot learn, so either cipher is a
 * possible console behavior.
 */
#ifndef RELIFT_ARM64_PAC_H
#define RELIFT_ARM64_PAC_H

#include <relift/arm64/abi.h>

/** @brief The keys as the lifted code reads them: the high then low half of IA, IB, DA, DB, GA. */
RL_RUNTIME_API extern uint64_t rl_pac_keys[10];

/** @brief The cipher rl_pac computes. */
typedef enum rl_pac_cipher {
    /** @brief Two rounds of a keyed 64x64->128 multiply-fold. */
    RL_PAC_FAST = 0,
    /** @brief QARMA5, exactly Arm ComputePACQARMA, through a per-thread memo. */
    RL_PAC_QARMA5 = 1,
} rl_pac_cipher;

/** @brief The cipher in use; set before guest code runs. */
RL_RUNTIME_API extern rl_pac_cipher rl_pac_mode;

/** @brief Selects the cipher and sets the ten key words. */
RL_RUNTIME_API void rl_pac_setup(rl_pac_cipher mode, const uint64_t keys[10]);

/** @brief Sets the ten key words from a 64-bit seed (splitmix64). */
RL_RUNTIME_API void rl_pac_seed(uint64_t seed);

/** @brief QARMA5 of data under modifier and key key0:key1, table driven. */
RL_RUNTIME_API uint64_t rl_pac_qarma5(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1);

/** @brief rl_pac_qarma5 through the calling thread's memo of recent results. */
RL_RUNTIME_API uint64_t rl_pac_qarma5_memo(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1);

/** @brief QARMA5 transcribed round by round from the Arm pseudocode, for tests. */
RL_RUNTIME_API uint64_t rl_pac_qarma5_reference(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1);

static inline uint64_t rl_pac_fold(uint64_t a, uint64_t b)
{
    rl_u128 p = (rl_u128)a * b;
    return (uint64_t)p ^ (uint64_t)(p >> 64);
}

/** @brief The RL_PAC_FAST cipher. */
static inline uint64_t rl_pac_fast(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1)
{
    uint64_t x = rl_pac_fold(data ^ key0, modifier ^ key1);
    return rl_pac_fold(x ^ key1 ^ UINT64_C(0x9e3779b97f4a7c15), data ^ modifier ^ key0 ^ UINT64_C(0xd6e8feb86659fd93));
}

/** @brief The IR operation pac: ComputePAC(data, modifier, key0, key1) with the selected cipher. */
static inline uint64_t rl_pac(uint64_t data, uint64_t modifier, uint64_t key0, uint64_t key1)
{
    if (__builtin_expect(rl_pac_mode == RL_PAC_FAST, 1))
        return rl_pac_fast(data, modifier, key0, key1);
    return rl_pac_qarma5_memo(data, modifier, key0, key1);
}

#endif
