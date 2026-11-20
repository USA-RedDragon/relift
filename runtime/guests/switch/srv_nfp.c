/**
 * @file srv_nfp.c
 * @brief The NFC service nfp:user (nn::nfc::detail::IUserManager and IUser,
 * which a Switch 2 SDK's nn::nfp client opens) of a console with no tag in
 * its reader: a client initializes and finalizes, lists no devices and
 * learns the interface state; a command on a device handle fails as for a
 * handle never given out. Command ids come from the SDK's own client (the
 * nn::nfp proxy tables, numbered as switchbrew.org lists nfp:user). The commands a game calls that
 * this does not answer stop with relift's unknown command report.
 */
#include "hos.h"
#include "services.h"

/** @brief nn::nfp State: the interface is initialized. */
#define NFP_STATE_INIT 1

/** @brief The nfp error module and the result for a device handle no reader gave out. */
#define NFP_MODULE 115
#define NFP_RESULT_DEVICE_NOT_FOUND RL_MAKE_RESULT(NFP_MODULE, 64)

/** @brief GetState (19): a u32, initialized (inferred: the SDK's Initialize succeeded before it asks). */
static rl_result user_get_state(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = NFP_STATE_INIT;
    return KR_SUCCESS;
}

/** @brief ListDevices (2): the device handles as a receive buffer and an i32 count, none: no reader has a tag in it. */
static rl_result user_list_devices(rl_ipc_req *r)
{
    return rl_ipc_out(r, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief AttachAvailabilityChangeEvent (23): a copy handle of an event that never fires, as the reader never changes. */
static rl_result user_attach_availability_event(rl_ipc_req *r)
{
    hos_revent *e = rl_hos_event_create();
    if (!e)
        return KR_OUT_OF_RESOURCE;
    rl_result rc = rl_ipc_push_copy(r, &e->obj);
    hos_kobj_unref(&e->obj);
    return rc;
}

/** @brief Commands on a device handle (3, 5, 13 to 16, 20, 21 and so on): this console never gave one out. */
static rl_result user_no_device(rl_ipc_req *r)
{
    (void)r;
    return NFP_RESULT_DEVICE_NOT_FOUND;
}

static const rl_ipc_cmd user_cmds[] = {
    { 0, "Initialize", rl_srv_stub_ok },
    { 1, "Finalize", rl_srv_stub_ok },
    { 2, "ListDevices", user_list_devices },
    { 3, "StartDetection", user_no_device },
    { 4, "StopDetection", user_no_device },
    { 5, "Mount", user_no_device },
    { 6, "Unmount", user_no_device },
    { 13, "GetTagInfo", user_no_device },
    { 14, "GetRegisterInfo", user_no_device },
    { 15, "GetCommonInfo", user_no_device },
    { 16, "GetModelInfo", user_no_device },
    { 17, "AttachActivateEvent", user_no_device },
    { 18, "AttachDeactivateEvent", user_no_device },
    { 19, "GetState", user_get_state },
    { 20, "GetDeviceState", user_no_device },
    { 21, "GetNpadId", user_no_device },
    { 23, "AttachAvailabilityChangeEvent", user_attach_availability_event },
};

static const rl_ipc_class user_class = { "nn::nfc::detail::IUser", user_cmds, sizeof user_cmds / sizeof user_cmds[0], 0,
                                         NULL };

/** @brief CreateUserInterface (0): an IUser. */
static rl_result create_user_interface(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_srv_object(&user_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "CreateUserInterface", create_user_interface },
};

static const rl_ipc_class manager_class = { "nn::nfc::detail::IUserManager", manager_cmds,
                                            sizeof manager_cmds / sizeof manager_cmds[0], 0, NULL };

static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_class, NULL); }

void rl_srv_install_nfp(void) { rl_ipc_register_service("nfp:user", manager_factory); }
