/**
 * @file srv_bcat.c
 * @brief The data delivery service bcat:u (nn::bcat::detail::ipc::IServiceCreator)
 * of a console that never downloaded any: the delivery cache is empty; and
 * the Switch 2's stream play service stpl:u (nn::streamplay::IUserService)
 * of a console that never streams. Command ids follow switchbrew.org (BCAT
 * services, Switch 2 Chat services).
 */
#include "hos.h"
#include "services.h"

#include <string.h>

static const rl_ipc_class bcat_class = { "nn::bcat::detail::ipc::IBcatService", NULL, 0, 0, NULL };

/** @brief EnumerateDeliveryCacheDirectory: a buffer of directory names; u32 count, always 0. */
static rl_result enumerate_directories(rl_ipc_req *r)
{
    int32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd storage_cmds[] = {
    { 10, "EnumerateDeliveryCacheDirectory", enumerate_directories },
};

static const rl_ipc_class storage_class = { "nn::bcat::detail::ipc::IDeliveryCacheStorageService", storage_cmds,
                                            sizeof storage_cmds / sizeof storage_cmds[0], 0, NULL };

static rl_result push(rl_ipc_req *r, const rl_ipc_class *cls)
{
    rl_ipc_object *o = rl_srv_object(cls, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static rl_result create_bcat_service(rl_ipc_req *r) { return push(r, &bcat_class); }

static rl_result create_storage_service(rl_ipc_req *r) { return push(r, &storage_class); }

static const rl_ipc_cmd creator_cmds[] = {
    { 0, "CreateBcatService", create_bcat_service },
    { 1, "CreateDeliveryCacheStorageService", create_storage_service },
    { 2, "CreateDeliveryCacheStorageServiceWithApplicationId", create_storage_service },
};

static const rl_ipc_class creator_class = { "bcat:u", creator_cmds, sizeof creator_cmds / sizeof creator_cmds[0], 0,
                                            NULL };

static rl_ipc_object *creator_factory(void) { return rl_srv_object(&creator_class, NULL); }

/**
 * @brief IUserShimScopedObject 1: u32 stream play type, u32 streaming mode,
 * s32; u32 bytes of memory stream play needs (read by
 * nn::streamplay::GetRequiredMemorySize). Without stream play, none.
 */
static rl_result required_memory_size(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/**
 * @brief IUserShimScopedObject 300: no input; a u32, a bool and a 16-byte
 * HidSharedMemoryConfig, which nn::streamplay::detail::GetUserService hands
 * to its HidApplicationResourceHolder. Without stream play, all zero.
 */
static rl_result hid_resource(rl_ipc_req *r)
{
    uint8_t *out = rl_ipc_out(r, 24);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    memset(out, 0, 24);
    return KR_SUCCESS;
}

static const rl_ipc_cmd shim_cmds[] = {
    { 1, "GetRequiredMemorySize", required_memory_size },
    { 300, "GetHidResource", hid_resource },
};

static const rl_ipc_class shim_class = { "nn::streamplay::IUserShimScopedObject", shim_cmds,
                                         sizeof shim_cmds / sizeof shim_cmds[0], 0, NULL };

static rl_result create_shim(rl_ipc_req *r) { return push(r, &shim_class); }

static const rl_ipc_cmd stpl_cmds[] = {
    { 0, "CreateUserShimScopedObject", create_shim },
};

static const rl_ipc_class stpl_class = { "stpl:u", stpl_cmds, sizeof stpl_cmds / sizeof stpl_cmds[0], 0, NULL };

static rl_ipc_object *stpl_factory(void) { return rl_srv_object(&stpl_class, NULL); }

void rl_srv_install_bcat(void)
{
    rl_ipc_register_service("bcat:u", creator_factory);
    if (hos_switch2())
        rl_ipc_register_service("stpl:u", stpl_factory);
}
