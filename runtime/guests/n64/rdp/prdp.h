/**
 * @file prdp.h
 * @brief The N64 RDP drawn by parallel-rdp (MIT, built from its pinned
 * upstream source as its own C++ unit, see NOTICE) on the host's Vulkan
 * device, behind the C renderer interface of n64.h.
 *
 * parallel-rdp works on RDRAM itself (RDRAM holds host-order 32-bit words,
 * its layout): the device imports it where it can and keeps its own copy in
 * step otherwise. At Sync_Full the machine waits for every command so far,
 * as ares does.
 */
#ifndef RELIFT_N64_PRDP_H
#define RELIFT_N64_PRDP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "n64.h"

/** @brief The parallel-rdp renderer. */
RL_RUNTIME_API extern const rl_n64_renderer rl_n64_prdp;

#ifdef __cplusplus
}
#endif

#endif
