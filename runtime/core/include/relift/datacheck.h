/**
 * @file datacheck.h
 * @brief Checks the game data a mobile app was given against the manifest relift wrote at build time.
 *
 * The manifest is text, one file per line: `<size> <sha256 hex> <root>/<path>`, where root is `romfs` or
 * `dlc` and names the folder RELIFT_ROMFS or RELIFT_DLC points at. Lines starting with `#` are comments.
 */
#ifndef RELIFT_DATACHECK_H
#define RELIFT_DATACHECK_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What a check found. */
typedef struct rl_data_report {
    /** @brief Files the manifest lists. */
    uint64_t files;
    /** @brief Their bytes. */
    uint64_t bytes;
    /** @brief Files that are missing, of another size or (with hashing) of other contents. */
    uint64_t bad;
    /** @brief The first bad file, as the manifest names it, or empty. */
    char first[512];
    /** @brief Why it is bad: "missing", "is N bytes, expected M" or "contents differ". */
    char why[96];
} rl_data_report;

/**
 * @brief Checks every file the manifest text lists: its size, and its SHA-256 when hash is true.
 * romfs and dlc are the folders the two roots stand for (NULL: every file under that root is missing).
 * Returns 0 when all match, 1 when a file does not (the report says which), -1 when the manifest is malformed.
 */
RL_RUNTIME_API int rl_data_check(const char *manifest, size_t len, const char *romfs, const char *dlc, bool hash,
                                 rl_data_report *out);

/**
 * @brief The check a mobile app runs at start (host/data_sdl3.c): reads data-manifest.txt from the app, checks the
 * folders RELIFT_ROMFS and RELIFT_DLC name (sizes; SHA-256 too with RELIFT_DATA_VERIFY=hash) and, on a mismatch,
 * logs and shows which file is missing or short. Returns 0 to boot (also when the app has no manifest), -1 not to.
 */
RL_RUNTIME_API int rl_app_check_data(void);

/** @brief SHA-256 of a file, as 64 lowercase hex digits and a NUL. Returns 0, or -1 when it cannot be read. */
RL_RUNTIME_API int rl_data_sha256_file(const char *path, char hex[65]);

/** @brief SHA-256 of a buffer, as 64 lowercase hex digits and a NUL. */
RL_RUNTIME_API void rl_data_sha256(const void *data, size_t n, char hex[65]);

#endif
