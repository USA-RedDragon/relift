/**
 * @file srv_link.h
 * @brief Hooks between service groups that are implemented in different files.
 */
#ifndef RELIFT_SRV_LINK_H
#define RELIFT_SRV_LINK_H

#include "hos.h"

/**
 * @brief Set by the vi group at install time. am calls it for
 * CreateManagedDisplayLayer; while NULL, am returns layer id 1.
 */
extern rl_result (*rl_srv_vi_layer_hook)(uint64_t *layer_id);

/** @brief Creates a managed display layer for the application. Defined by vi (am defines a default only under RL_SRV_VI_HOOK_DEFAULT). */
rl_result rl_srv_vi_create_managed_layer(uint64_t *layer_id);

#endif
