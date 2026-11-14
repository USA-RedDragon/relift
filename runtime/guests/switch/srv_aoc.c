/**
 * @file srv_aoc.c
 * @brief aoc:u, the add-on content manager (nn::aocsrv::detail::IAddOnContentManager).
 *
 * Command ids follow switchbrew.org (NS services) and Ryujinx (MIT)
 * Services/Ns/Aoc, read as specifications. The add-ons are the ones in the
 * add-on directory (hos_fs.h): an add-on's id is the add-on base id plus its
 * index, and its data is opened through fsp-srv by that id.
 */
#include "hos_fs.h"
#include "services.h"

#include <stdlib.h>
#include <string.h>

#define AOC_ID_MASK 0xFFFull
#define AOC_BASE_OFFSET 0x1000ull

static struct {
    hos_revent *list_changed;
    hos_revent *purchased;
} g_aoc;

static uint64_t base_id(uint64_t application_id) { return (application_id & ~AOC_ID_MASK) + AOC_BASE_OFFSET; }

static uint32_t indices_of(uint64_t base, uint32_t *out, uint32_t cap)
{
    size_t total = rl_hos_fs_addon_ids(NULL, 0);
    uint64_t *ids = total ? malloc(total * sizeof *ids) : NULL;
    uint32_t n = 0;
    if (total && !ids)
        return 0;
    total = ids ? rl_hos_fs_addon_ids(ids, total) : 0;
    for (size_t i = 0; i < total; i++) {
        uint64_t idx = ids[i] - base;
        if ((ids[i] & ~AOC_ID_MASK) != base || idx == 0 || idx > AOC_ID_MASK)
            continue;
        if (out && n < cap)
            out[n] = (uint32_t)idx;
        n++;
    }
    free(ids);
    return n;
}

static rl_result count(rl_ipc_req *r, uint64_t application_id)
{
    *(uint32_t *)rl_ipc_out(r, 4) = indices_of(base_id(application_id), NULL, 0);
    return KR_SUCCESS;
}

static rl_result list(rl_ipc_req *r, uint32_t offset, uint32_t want, uint64_t application_id)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    uint64_t base = base_id(application_id);
    uint32_t total = indices_of(base, NULL, 0);
    uint32_t room = b.size / 4 < want ? (uint32_t)(b.size / 4) : want;
    uint32_t n = offset < total ? total - offset : 0;
    uint32_t *all = NULL;
    if (n > room)
        n = room;
    if (n) {
        all = malloc((size_t)total * sizeof *all);
        if (!all)
            return KR_OUT_OF_RESOURCE;
        indices_of(base, all, total);
        if (!rl_ipc_buf_write(&b, all + offset, (uint64_t)n * 4)) {
            free(all);
            return SF_RESULT_INVALID_HEADER;
        }
        free(all);
    }
    *(uint32_t *)rl_ipc_out(r, 4) = n;
    return KR_SUCCESS;
}

static bool in_u64(const rl_ipc_req *r, uint32_t off, uint64_t *v)
{
    if (r->in_size < off + 8)
        return false;
    memcpy(v, r->in + off, 8);
    return true;
}

static rl_result count_by_application_id(rl_ipc_req *r)
{
    uint64_t app;
    if (!in_u64(r, 0, &app))
        return SF_RESULT_INVALID_HEADER;
    return count(r, app);
}

static rl_result count_for_process(rl_ipc_req *r) { return count(r, hos.cfg.program_id); }

static rl_result list_by_application_id(rl_ipc_req *r)
{
    uint32_t v[2];
    uint64_t app;
    if (!rl_srv_in(r, v, sizeof v) || !in_u64(r, 8, &app))
        return SF_RESULT_INVALID_HEADER;
    return list(r, v[0], v[1], app);
}

static rl_result list_for_process(rl_ipc_req *r)
{
    uint32_t v[2];
    if (!rl_srv_in(r, v, sizeof v))
        return SF_RESULT_INVALID_HEADER;
    return list(r, v[0], v[1], hos.cfg.program_id);
}

static rl_result base_id_by_application_id(rl_ipc_req *r)
{
    uint64_t app;
    if (!in_u64(r, 0, &app))
        return SF_RESULT_INVALID_HEADER;
    *(uint64_t *)rl_ipc_out(r, 8) = base_id(app);
    return KR_SUCCESS;
}

static rl_result base_id_for_process(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = base_id(hos.cfg.program_id);
    return KR_SUCCESS;
}

static rl_result event(rl_ipc_req *r, hos_revent **slot)
{
    if (!*slot)
        *slot = rl_hos_event_create();
    if (!*slot)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &(*slot)->obj);
}

static rl_result list_changed_event(rl_ipc_req *r) { return event(r, &g_aoc.list_changed); }

static rl_result purchase_event(rl_ipc_req *r) { return event(r, &g_aoc.purchased); }

static const rl_ipc_cmd purchase_cmds[] = {
    RL_SRV_STUB(0, "SetDefaultDeliveryTarget"),
    RL_SRV_STUB(1, "SetDeliveryTarget"),
    { 2, "GetPurchasedEventReadableHandle", purchase_event },
};

static const rl_ipc_class purchase_class = { "nn::ec::IPurchaseEventManager", purchase_cmds,
                                             sizeof purchase_cmds / sizeof purchase_cmds[0], 0, NULL };

static rl_result create_purchase_manager(rl_ipc_req *r)
{
    return rl_ipc_push_object(r, rl_srv_object(&purchase_class, NULL));
}

static const rl_ipc_cmd aoc_cmds[] = {
    { 0, "CountAddOnContentByApplicationId", count_by_application_id },
    { 1, "ListAddOnContentByApplicationId", list_by_application_id },
    { 2, "CountAddOnContent", count_for_process },
    { 3, "ListAddOnContent", list_for_process },
    { 4, "GetAddOnContentBaseIdByApplicationId", base_id_by_application_id },
    { 5, "GetAddOnContentBaseId", base_id_for_process },
    RL_SRV_STUB(6, "PrepareAddOnContentByApplicationId"),
    RL_SRV_STUB(7, "PrepareAddOnContent"),
    { 8, "GetAddOnContentListChangedEvent", list_changed_event },
    { 10, "GetAddOnContentListChangedEventWithProcessId", list_changed_event },
    RL_SRV_STUB(11, "NotifyMountAddOnContent"),
    RL_SRV_STUB(12, "NotifyUnmountAddOnContent"),
    RL_SRV_STUB(50, "CheckAddOnContentMountStatus"),
    { 100, "CreateEcPurchasedEventManager", create_purchase_manager },
    { 101, "CreatePermanentEcPurchasedEventManager", create_purchase_manager },
};

static const rl_ipc_class aoc_class = { "aoc:u", aoc_cmds, sizeof aoc_cmds / sizeof aoc_cmds[0], 0, NULL };

static rl_ipc_object *aoc_factory(void) { return rl_srv_object(&aoc_class, NULL); }

void rl_srv_install_aoc(void)
{
    hos_revent *ev[2] = { g_aoc.list_changed, g_aoc.purchased };
    for (int i = 0; i < 2; i++)
        if (ev[i])
            hos_kobj_unref(&ev[i]->obj);
    memset(&g_aoc, 0, sizeof g_aoc);
    rl_ipc_register_service("aoc:u", aoc_factory);
}
