/**
 * @file override.h
 * @brief The header an override includes: settings reads and port input.
 *
 * A game declares its settings in relift.toml (`[settings.<key>]`). An
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
#include <relift/ports.h>

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
/** @brief The host inputs of a key-binding setting, as list text such as "gamepad:south,key:z". */
RL_RUNTIME_API const char *relift_setting_binding(const char *key);
/** @brief The current value of a key-binding setting: its inputs read now, 0 to 1 (button) or -1 to 1. */
RL_RUNTIME_API float relift_setting_binding_value(const char *key);
/**
 * @brief The current value of a control of a port: a button is 0 or 1, a
 * trigger 0 to 1 and an axis -1 to 1. An unknown port or control reads 0.
 * For a control read every frame, keep the result of rl_ports_find and call
 * rl_ports_read.
 */
RL_RUNTIME_API float relift_input(const char *port, const char *control);

#ifdef __cplusplus
}
#endif

#endif
