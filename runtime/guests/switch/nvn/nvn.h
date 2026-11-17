/**
 * @file nvn.h
 * @brief NVN2 (the Switch 2's graphics API) replaced by name.
 *
 * Every Switch 2 game reaches NVN2 through nvnBootstrapLoader, which the
 * SDK's sdk module defines and the game's modules import. The loader hook of
 * this layer replaces nvnBootstrapLoader in every module, so the game gets
 * relift's nvnDeviceGetProcAddress and, from it, relift's function for every
 * proc name; the SDK's own NVN2 driver never runs. Each name gets a guest
 * address in a page of thunks next to the lifted code. A name relift does
 * not implement yet still gets an address, and a call to it stops the game
 * with a report that names the proc.
 */
#ifndef RELIFT_SWITCH_NVN_H
#define RELIFT_SWITCH_NVN_H

#include "core.h"
#include "loader.h"

#include <stdint.h>

/** @brief Most proc names the layer hands out addresses for (implemented and not). */
#define RL_NVN_SLOTS 1024u

/** @brief The loader hook; it adds itself to a program whose modules define or import nvnBootstrapLoader. */
RL_RUNTIME_API const rl_switch_hook *rl_nvn_switch_hook(void);

/** @brief The guest address of proc name, or 0 when the layer is not up or its slots are all taken. */
RL_RUNTIME_API uint64_t rl_nvn_proc(const char *name);

/** @brief The name of the proc at guest address addr, or NULL. */
RL_RUNTIME_API const char *rl_nvn_proc_name(uint64_t addr);

#endif
