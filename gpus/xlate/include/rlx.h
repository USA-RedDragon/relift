/**
 * @file rlx.h
 * @brief The NVIDIA GPU program translator, for the runtime to make a game's shader tables on the player's device.
 *
 * librelift_gpu_xlate.a (gpus/xlate) runs the translator relift build runs, so the tables it writes are
 * byte for byte the ones the build machine makes for the same programs and device description. It reads and writes
 * only the paths given here and opens no network connection.
 *
 * A run: rlx_new with the game's programs (the package's programs.rlsp, or a build's shader folder) and the bytes of
 * the device.json the runtime wrote, then rlx_write_tables from a worker thread while the compile screen shows the
 * progress, then rlx_free. Every call catches a translator panic and reports it as an error.
 */
#ifndef RELIFT_RLX_H
#define RELIFT_RLX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The ABI version this header describes; rlx_abi() of the library must equal it. */
#define RLX_ABI 1

/** @brief A device's translator and the programs it translates. */
typedef struct rlx rlx;

/** @brief What the tables hold. */
typedef struct rlx_built {
    /** @brief Programs given. */
    uint32_t programs;
    /** @brief Programs translated. */
    uint32_t translated;
    /** @brief Programs the translator refused (listed in failed.txt). */
    uint32_t refused;
} rlx_built;

/**
 * @brief The progress of rlx_write_tables: programs done and in all, after each batch, on the thread that called
 * rlx_write_tables. Returning 0 stops the work.
 */
typedef int (*rlx_progress)(void *user, uint32_t done, uint32_t total);

/** @brief The library's ABI version (RLX_ABI). */
uint32_t rlx_abi(void);

/** @brief The translator stamp (16 hex digits): the one every table this library writes carries in table.stamp. */
const char *rlx_stamp(void);

/**
 * @brief A translator for the device that device (device_len bytes of the runtime's device.json) describes, over the
 * programs at the path programs: a program pack file (programs.rlsp) or a shader folder with programs/ and
 * meshes.json; NULL for none. NULL on an error, with the reason in why (why_len bytes, may be NULL).
 */
rlx *rlx_new(const char *programs, const uint8_t *device, size_t device_len, char *why, size_t why_len);

/** @brief Frees a translator; NULL is allowed. */
void rlx_free(rlx *x);

/** @brief The number of programs the translator has. */
uint32_t rlx_programs(const rlx *x);

/**
 * @brief Translates one program (its SPH, if any, then its code: len bytes at code) as a table holds it. On success 0
 * and *words is the SPIR-V module of *n_words words, freed with rlx_words_free; on a refusal or an error -1 with the
 * reason in why. Safe from several threads at once.
 */
int rlx_translate_one(const rlx *x, const uint8_t *code, size_t len, uint32_t **words, size_t *n_words, char *why,
                      size_t why_len);

/** @brief Frees a module from rlx_translate_one; NULL is allowed. */
void rlx_words_free(uint32_t *words, size_t n_words);

/**
 * @brief Translates every program of x on threads worker threads (0 for one per CPU) and writes in the folder dir
 * (made if missing) table.rlspv, table.rlspi, table.stamp, table.device (the device description) and failed.txt.
 * progress may be NULL; a stopped run leaves the old tables. out may be NULL. 0 on success, else -1 with the reason
 * in why.
 */
int rlx_write_tables(const rlx *x, const char *dir, uint32_t threads, rlx_progress progress, void *user,
                     rlx_built *out, char *why, size_t why_len);

#ifdef __cplusplus
}
#endif

#endif
