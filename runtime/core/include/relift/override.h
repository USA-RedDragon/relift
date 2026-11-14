/**
 * @file override.h
 * @brief The header an override includes: settings reads.
 *
 * A game declares its settings in game.toml (`[settings.<key>]`). An
 * override reads them with the functions below and a string literal key.
 * `relift build` scans the override sources and fails the build when a key
 * is not declared, or is read as another type than it was declared with, so
 * a misspelled key never reaches a run.
 *
 * Values are fixed once the program has started: the launcher passes them
 * before the first guest instruction runs (apply = "start").
 */
#ifndef RELIFT_OVERRIDE_H
#define RELIFT_OVERRIDE_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The value of a bool setting. */
RL_RUNTIME_API bool relift_setting_bool(const char *key);
/** @brief The value of an int setting. */
RL_RUNTIME_API int64_t relift_setting_int(const char *key);
/** @brief The value of a float setting. */
RL_RUNTIME_API double relift_setting_float(const char *key);
/** @brief The index into the declared choices of an enum setting. */
RL_RUNTIME_API uint32_t relift_setting_enum(const char *key);
/** @brief The text of a string setting; valid for the run. */
RL_RUNTIME_API const char *relift_setting_str(const char *key);
/**
 * @brief The port and control names of a key-binding setting. Returns false
 * and sets both to "" when the binding has no value.
 */
RL_RUNTIME_API bool relift_setting_binding(const char *key, const char **port, const char **control);

#ifdef __cplusplus
}
#endif

#endif
