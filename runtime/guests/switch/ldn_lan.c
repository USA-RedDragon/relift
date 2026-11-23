/**
 * @file ldn_lan.c
 * @brief The ldn_mitm LAN protocol backend of local wireless play. Until the
 * protocol lands it does not start, and the service falls back to offline.
 */
#include "ldn.h"

#include <stddef.h>

ldn_backend *rl_ldn_lan_backend(ldn_change_fn *change, void *ctx)
{
    (void)change;
    (void)ctx;
    return NULL;
}
