/**
 * @file srv_hid_sys.c
 * @brief hid:sys, hid:dbg, irs and irs:sys.
 *
 * Command ids, behavior and results follow Ryujinx (MIT, Copyright (c)
 * Ryujinx Team and Contributors) Hid/IHidSystemServer.cs, IHidDebugServer.cs
 * and Irs/IIrSensorServer.cs.
 */
#include "hid_priv.h"

#include <string.h>

#define IRS_SHMEM_SIZE 0x8000u

static hos_shmem *g_irs_shm;

static rl_result sys_get_ext_style(rl_ipc_req *r)
{
    uint64_t *o = rl_ipc_out(r, 8);
    *o = hid_core_styles();
    return KR_SUCCESS;
}

static uint8_t footer_of(uint32_t id)
{
    uint32_t slot;
    if (!hid_slot_from_id(id, &slot))
        slot = 0;
    return hid_core_footer_type(slot);
}

static rl_result sys_get_last_active_npad(rl_ipc_req *r)
{
    uint32_t in[2] = { 0, 0 };
    rl_srv_in(r, in, sizeof in);
    uint8_t *o = rl_ipc_out(r, 2);
    o[0] = footer_of(in[1]);
    return KR_SUCCESS;
}

static rl_result sys_get_footer_type(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, sizeof id))
        return SF_RESULT_INVALID_HEADER;
    uint32_t slot;
    if (!hid_slot_from_id(id, &slot))
        return HID_RESULT_INVALID_NPAD_ID_TYPE;
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = hid_core_footer_type(slot);
    return KR_SUCCESS;
}

static const rl_ipc_cmd sys_cmds[] = {
    RL_SRV_STUB(303, "ApplyNpadSystemCommonPolicy"),
    { 306, "GetLastActiveNpad", sys_get_last_active_npad },
    { 307, "GetNpadSystemExtStyle", sys_get_ext_style },
    { 314, "GetAppletFooterUiType", sys_get_footer_type },
};

static const rl_ipc_class sys_cls = { "hid:sys", sys_cmds, (uint32_t)(sizeof sys_cmds / sizeof sys_cmds[0]), 0, NULL };

static rl_ipc_object *sys_factory(void) { return rl_srv_object(&sys_cls, NULL); }

static const rl_ipc_class dbg_cls = { "hid:dbg", NULL, 0, 0, NULL };

static rl_ipc_object *dbg_factory(void) { return rl_srv_object(&dbg_cls, NULL); }

static rl_result irs_get_shared_memory_handle(rl_ipc_req *r)
{
    if (!g_irs_shm) {
        g_irs_shm = rl_hos_shmem_create(IRS_SHMEM_SIZE, HOS_PERM_R);
        if (!g_irs_shm)
            return KR_OUT_OF_MEMORY;
    }
    return rl_ipc_push_copy(r, &g_irs_shm->obj);
}

static rl_result irs_get_image_transfer_state(rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (b.size == 0)
        return IRS_RESULT_INVALID_BUFFER_SIZE;
    rl_ipc_out(r, 16);
    return KR_SUCCESS;
}

static rl_result irs_get_npad_camera_handle(rl_ipc_req *r)
{
    uint32_t id;
    if (!rl_srv_in(r, &id, sizeof id))
        return SF_RESULT_INVALID_HEADER;
    uint32_t slot;
    if (!hid_slot_from_id(id, &slot))
        return IRS_RESULT_NPAD_ID_OUT_OF_RANGE;
    int32_t *o = rl_ipc_out(r, 4);
    *o = slot == 9 ? 9 : (int32_t)slot;
    return KR_SUCCESS;
}

static const rl_ipc_cmd irs_cmds[] = {
    RL_SRV_STUB(302, "ActivateIrsensor"),
    RL_SRV_STUB(303, "DeactivateIrsensor"),
    { 304, "GetIrsensorSharedMemoryHandle", irs_get_shared_memory_handle },
    RL_SRV_STUB(305, "StopImageProcessor"),
    RL_SRV_STUB(306, "RunMomentProcessor"),
    RL_SRV_STUB(307, "RunClusteringProcessor"),
    RL_SRV_STUB(308, "RunImageTransferProcessor"),
    { 309, "GetImageTransferProcessorState", irs_get_image_transfer_state },
    RL_SRV_STUB(310, "RunTeraPluginProcessor"),
    { 311, "GetNpadIrCameraHandle", irs_get_npad_camera_handle },
    RL_SRV_STUB(314, "CheckFirmwareVersion"),
    RL_SRV_STUB(318, "StopImageProcessorAsync"),
    RL_SRV_STUB(319, "ActivateIrsensorWithFunctionLevel"),
};

static const rl_ipc_class irs_cls = { "irs", irs_cmds, (uint32_t)(sizeof irs_cmds / sizeof irs_cmds[0]), 0, NULL };

static rl_ipc_object *irs_factory(void) { return rl_srv_object(&irs_cls, NULL); }

static const rl_ipc_class irs_sys_cls = { "irs:sys", NULL, 0, 0, NULL };

static rl_ipc_object *irs_sys_factory(void) { return rl_srv_object(&irs_sys_cls, NULL); }

void hid_sys_install(void)
{
    if (g_irs_shm) {
        hos_kobj_unref(&g_irs_shm->obj);
        g_irs_shm = NULL;
    }
    rl_ipc_register_service("hid:sys", sys_factory);
    rl_ipc_register_service("hid:dbg", dbg_factory);
    rl_ipc_register_service("irs", irs_factory);
    rl_ipc_register_service("irs:sys", irs_sys_factory);
}
