/**
 * @file srv_hid.c
 * @brief The hid service (IHidServer), IAppletResource and the active
 * vibration device list.
 *
 * Command ids, behavior and results follow Ryujinx (MIT, Copyright (c)
 * Ryujinx Team and Contributors); request layouts follow libnx (ISC,
 * Copyright (c) libnx Authors) services/hid.c.
 */
#include "hid_priv.h"

#include <stdlib.h>
#include <string.h>

typedef struct hid_srv {
    hos_revent *xpad_event;
    hos_revent *palma_event;
    bool fusion_enabled;
    bool home_protection;
    bool center_clamp;
    bool vibration_permitted;
    bool usb_full_key;
    bool unaltered_passthrough;
    uint64_t handheld_mode;
    uint64_t comm_mode;
    uint32_t gyro_mode;
    uint32_t accel_play_mode;
    float seven_strength;
    float fusion_params[2];
    float accel_params[2];
} hid_srv;

typedef struct in_aruid {
    uint64_t aruid;
} in_aruid;

typedef struct in_u32_aruid {
    uint32_t v;
    uint32_t pad;
    uint64_t aruid;
} in_u32_aruid;

typedef struct in_aruid_u64 {
    uint64_t aruid;
    uint64_t v;
} in_aruid_u64;

typedef struct in_u32_aruid_u64 {
    uint32_t v;
    uint32_t pad;
    uint64_t aruid;
    uint64_t v64;
} in_u32_aruid_u64;

typedef struct in_handle_u32_aruid {
    uint32_t handle;
    uint32_t value;
    uint64_t aruid;
} in_handle_u32_aruid;

typedef struct in_handle_f32x2 {
    uint32_t handle;
    float a, b;
    uint32_t pad;
    uint64_t aruid;
} in_handle_f32x2;

typedef struct in_flag_u32_aruid {
    uint8_t flag;
    uint8_t pad[3];
    uint32_t v;
    uint64_t aruid;
} in_flag_u32_aruid;

/**
 * @brief The SDK sends its X buffers (the npad id list of
 * SetSupportedNpadIdType) through the server's pointer buffer and aborts with
 * hipc PointerBufferTooSmall when QueryPointerBufferSize answers 0.
 */
#define HID_POINTER_BUFFER_SIZE 0x1000

#define ST(r) ((hid_srv *)(r)->self->state)

#define NEED(r, T, name)                          \
    T name;                                       \
    if (!rl_srv_in((r), &name, sizeof name))      \
    return SF_RESULT_INVALID_HEADER

#define STUB_OUT(id, name, fn) { (id), name "*", fn }

static rl_result out_zero8(rl_ipc_req *r)
{
    rl_ipc_out(r, 8);
    return KR_SUCCESS;
}

static rl_result out_true(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = 1;
    return KR_SUCCESS;
}

static rl_result out_two_true(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 2);
    o[0] = 1;
    o[1] = 1;
    return KR_SUCCESS;
}

static rl_result out_false(rl_ipc_req *r)
{
    rl_ipc_out(r, 1);
    return KR_SUCCESS;
}

static rl_result out_zero2(rl_ipc_req *r)
{
    rl_ipc_out(r, 2);
    return KR_SUCCESS;
}

static rl_result out_zero1(rl_ipc_req *r)
{
    rl_ipc_out(r, 1);
    return KR_SUCCESS;
}

typedef struct applet_resource {
    hos_shmem *shm;
} applet_resource;

static rl_result ar_get_shared_memory_handle(rl_ipc_req *r)
{
    applet_resource *a = r->self->state;
    return rl_ipc_push_copy(r, &a->shm->obj);
}

static void ar_destroy(void *state)
{
    applet_resource *a = state;
    hos_kobj_unref(&a->shm->obj);
    free(a);
}

static const rl_ipc_cmd ar_cmds[] = {
    { 0, "GetSharedMemoryHandle", ar_get_shared_memory_handle },
};

static const rl_ipc_class ar_cls = { "IAppletResource", ar_cmds, 1, 0, ar_destroy };

static const rl_ipc_cmd avdl_cmds[] = {
    RL_SRV_STUB(0, "ActivateVibrationDevice"),
};

static const rl_ipc_class avdl_cls = { "IActiveVibrationDeviceList", avdl_cmds, 1, 0, NULL };

static rl_result create_applet_resource(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    hos_shmem *shm = hid_core_open();
    if (!shm)
        return KR_OUT_OF_MEMORY;
    applet_resource *a = calloc(1, sizeof *a);
    if (!a) {
        hos_kobj_unref(&shm->obj);
        return KR_OUT_OF_MEMORY;
    }
    a->shm = shm;
    return rl_ipc_push_object(r, rl_srv_object(&ar_cls, a));
}

#define ACTIVATE(fn, dev)                    \
    static rl_result fn(rl_ipc_req *r)       \
    {                                        \
        NEED(r, in_aruid, in);               \
        hid_core_activate(dev);              \
        return KR_SUCCESS;                   \
    }

ACTIVATE(activate_debug_pad, HID_DEV_DEBUG_PAD)
ACTIVATE(activate_touch_screen, HID_DEV_TOUCH)
ACTIVATE(activate_mouse, HID_DEV_MOUSE)
ACTIVATE(activate_debug_mouse, HID_DEV_DEBUG_MOUSE)
ACTIVATE(activate_keyboard, HID_DEV_KEYBOARD)

static rl_result activate_npad(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    hid_core_activate(HID_DEV_NPAD);
    return KR_SUCCESS;
}

static rl_result activate_npad_with_revision(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    hid_core_activate(HID_DEV_NPAD);
    return KR_SUCCESS;
}

static rl_result acquire_xpad_id_event_handle(rl_ipc_req *r)
{
    hid_srv *s = ST(r);
    return rl_ipc_push_copy(r, &s->xpad_event->obj);
}

static rl_result get_fusion_enabled(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = ST(r)->fusion_enabled;
    return KR_SUCCESS;
}

static rl_result enable_fusion(rl_ipc_req *r)
{
    NEED(r, in_flag_u32_aruid, in);
    ST(r)->fusion_enabled = in.flag != 0;
    return KR_SUCCESS;
}

static rl_result set_fusion_params(rl_ipc_req *r)
{
    NEED(r, in_handle_f32x2, in);
    ST(r)->fusion_params[0] = in.a;
    ST(r)->fusion_params[1] = in.b;
    return KR_SUCCESS;
}

static rl_result get_fusion_params(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    float *o = rl_ipc_out(r, 8);
    memcpy(o, ST(r)->fusion_params, 8);
    return KR_SUCCESS;
}

static rl_result reset_fusion_params(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    memset(ST(r)->fusion_params, 0, 8);
    return KR_SUCCESS;
}

static rl_result set_accel_params(rl_ipc_req *r)
{
    NEED(r, in_handle_f32x2, in);
    ST(r)->accel_params[0] = in.a;
    ST(r)->accel_params[1] = in.b;
    return KR_SUCCESS;
}

static rl_result get_accel_params(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    float *o = rl_ipc_out(r, 8);
    memcpy(o, ST(r)->accel_params, 8);
    return KR_SUCCESS;
}

static rl_result reset_accel_params(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    memset(ST(r)->accel_params, 0, 8);
    return KR_SUCCESS;
}

static rl_result set_accel_play_mode(rl_ipc_req *r)
{
    NEED(r, in_handle_u32_aruid, in);
    ST(r)->accel_play_mode = in.value;
    return KR_SUCCESS;
}

static rl_result get_accel_play_mode(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint32_t *o = rl_ipc_out(r, 4);
    *o = ST(r)->accel_play_mode;
    return KR_SUCCESS;
}

static rl_result reset_accel_play_mode(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    ST(r)->accel_play_mode = 0;
    return KR_SUCCESS;
}

static rl_result set_gyro_mode(rl_ipc_req *r)
{
    NEED(r, in_handle_u32_aruid, in);
    ST(r)->gyro_mode = in.value;
    return KR_SUCCESS;
}

static rl_result get_gyro_mode(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint32_t *o = rl_ipc_out(r, 4);
    *o = ST(r)->gyro_mode;
    return KR_SUCCESS;
}

static rl_result reset_gyro_mode(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    ST(r)->gyro_mode = 1;
    return KR_SUCCESS;
}

static rl_result enable_unaltered_passthrough(rl_ipc_req *r)
{
    NEED(r, in_flag_u32_aruid, in);
    ST(r)->unaltered_passthrough = in.flag != 0;
    return KR_SUCCESS;
}

static rl_result get_unaltered_passthrough(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = ST(r)->unaltered_passthrough;
    return KR_SUCCESS;
}

static rl_result set_supported_style_set(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    hid_core_set_styles(in.v);
    return KR_SUCCESS;
}

static rl_result get_supported_style_set(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    uint32_t *o = rl_ipc_out(r, 4);
    *o = hid_core_styles();
    return KR_SUCCESS;
}

static rl_result set_supported_id_type(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    uint32_t ids[64];
    uint32_t n = (uint32_t)(b.size / 4);
    if (n > 64)
        n = 64;
    if (n && !rl_ipc_buf_read(&b, ids, (uint64_t)n * 4))
        return HID_RESULT_INVALID_BUFFER_SIZE;
    hid_core_set_players(ids, n);
    return KR_SUCCESS;
}

static rl_result acquire_style_set_update_event(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint32_t slot;
    if (!hid_slot_from_id(in.v, &slot) || slot > HID_NPAD_HANDHELD)
        return HID_RESULT_INVALID_NPAD_ID_TYPE;
    hos_revent *e = hid_core_style_event(slot);
    if (!e)
        return KR_OUT_OF_MEMORY;
    rl_hos_event_signal(e);
    return rl_ipc_push_copy(r, &e->obj);
}

static rl_result get_player_led_pattern(rl_ipc_req *r)
{
    NEED(r, uint32_t, id);
    static const uint64_t pattern[] = { 1, 3, 7, 15, 9, 5, 13, 6 };
    uint64_t v;
    if (id <= HID_ID_PLAYER8)
        v = pattern[id];
    else if (id == HID_ID_UNKNOWN || id == HID_ID_HANDHELD)
        v = 0;
    else
        return HID_RESULT_INVALID_NPAD_ID_TYPE;
    uint64_t *o = rl_ipc_out(r, 8);
    *o = v;
    return KR_SUCCESS;
}

static rl_result set_joy_hold_type(rl_ipc_req *r)
{
    NEED(r, in_aruid_u64, in);
    if (in.v > 1)
        return HID_RESULT_INVALID_NPAD_DEVICE_TYPE;
    hid_core_set_joy_hold(in.v);
    return KR_SUCCESS;
}

static rl_result get_joy_hold_type(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    uint64_t *o = rl_ipc_out(r, 8);
    *o = hid_core_joy_hold();
    return KR_SUCCESS;
}

static rl_result assign_single(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint32_t slot;
    if (hid_slot_from_id(in.v, &slot))
        hid_core_set_assignment(slot, 1);
    return KR_SUCCESS;
}

static rl_result assign_dual(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint32_t slot;
    if (hid_slot_from_id(in.v, &slot))
        hid_core_set_assignment(slot, 0);
    return KR_SUCCESS;
}

static rl_result assign_single_with_destination(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid_u64, in);
    uint32_t slot;
    if (hid_slot_from_id(in.v, &slot))
        hid_core_set_assignment(slot, 1);
    rl_ipc_out(r, 8);
    return KR_SUCCESS;
}

static rl_result set_handheld_activation_mode(rl_ipc_req *r)
{
    NEED(r, in_aruid_u64, in);
    ST(r)->handheld_mode = in.v;
    return KR_SUCCESS;
}

static rl_result get_handheld_activation_mode(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    uint64_t *o = rl_ipc_out(r, 8);
    *o = ST(r)->handheld_mode;
    return KR_SUCCESS;
}

static rl_result get_home_protection(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = ST(r)->home_protection;
    return KR_SUCCESS;
}

static rl_result set_home_protection(rl_ipc_req *r)
{
    NEED(r, in_flag_u32_aruid, in);
    ST(r)->home_protection = in.flag != 0;
    return KR_SUCCESS;
}

static rl_result set_center_clamp(rl_ipc_req *r)
{
    NEED(r, in_flag_u32_aruid, in);
    ST(r)->center_clamp = in.flag != 0;
    return KR_SUCCESS;
}

typedef struct vib_handle {
    uint8_t device_type, player_id, position, reserved;
} vib_handle;

static rl_result get_vibration_device_info(rl_ipc_req *r)
{
    NEED(r, vib_handle, h);
    uint32_t slot;
    if (!hid_slot_from_id(h.player_id, &slot))
        return HID_RESULT_INVALID_NPAD_ID_TYPE;
    if (h.position > 1)
        return HID_RESULT_INVALID_DEVICE_INDEX;
    uint32_t type = 0;
    uint32_t pos = 0;
    if ((h.device_type >= 3 && h.device_type <= 7) || h.device_type == 32 || h.device_type == 33) {
        type = 1;
        pos = h.position == 0 ? 1 : 2;
    } else if (h.device_type == 8) {
        type = 2;
    }
    uint32_t *o = rl_ipc_out(r, 8);
    o[0] = type;
    o[1] = pos;
    return KR_SUCCESS;
}

typedef struct in_send_vibration {
    vib_handle handle;
    float value[4];
    uint32_t pad;
    uint64_t aruid;
} in_send_vibration;

static rl_result send_vibration_value(rl_ipc_req *r)
{
    NEED(r, in_send_vibration, in);
    uint32_t slot;
    if (hid_slot_from_id(in.handle.player_id, &slot))
        hid_core_set_vibration(slot, in.handle.position, in.value);
    return KR_SUCCESS;
}

static rl_result get_actual_vibration_value(rl_ipc_req *r)
{
    NEED(r, in_u32_aruid, in);
    vib_handle h;
    memcpy(&h, &in.v, sizeof h);
    uint32_t slot = 0;
    float v[4];
    if (hid_slot_from_id(h.player_id, &slot))
        hid_core_get_vibration(slot, h.position, v);
    else
        hid_core_get_vibration(HID_SLOT_NONE, 0, v);
    float *o = rl_ipc_out(r, 16);
    memcpy(o, v, 16);
    return KR_SUCCESS;
}

static rl_result create_active_vibration_device_list(rl_ipc_req *r)
{
    return rl_ipc_push_object(r, rl_srv_object(&avdl_cls, NULL));
}

static rl_result permit_vibration(rl_ipc_req *r)
{
    uint8_t v = 0;
    if (!rl_srv_in(r, &v, 1))
        return SF_RESULT_INVALID_HEADER;
    ST(r)->vibration_permitted = v != 0;
    return KR_SUCCESS;
}

static rl_result is_vibration_permitted(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = ST(r)->vibration_permitted;
    return KR_SUCCESS;
}

static rl_result send_vibration_values(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    rl_ipc_buf hb = rl_ipc_auto_in(r, 0);
    rl_ipc_buf vb = rl_ipc_auto_in(r, 1);
    uint64_t n = hb.size / sizeof(vib_handle);
    if (n == 0 || vb.size / 16 != n)
        return KR_SUCCESS;
    if (n > 32)
        n = 32;
    vib_handle h[32];
    float v[32][4];
    if (!rl_ipc_buf_read(&hb, h, n * sizeof h[0]) || !rl_ipc_buf_read(&vb, v, n * 16))
        return KR_SUCCESS;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t slot;
        if (hid_slot_from_id(h[i].player_id, &slot))
            hid_core_set_vibration(slot, h[i].position, v[i]);
    }
    return KR_SUCCESS;
}

static rl_result set_seven_strength(rl_ipc_req *r)
{
    struct {
        float strength;
        uint32_t pad;
        uint64_t aruid;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    ST(r)->seven_strength = in.strength;
    return KR_SUCCESS;
}

static rl_result get_seven_strength(rl_ipc_req *r)
{
    NEED(r, in_aruid, in);
    float *o = rl_ipc_out(r, 4);
    *o = ST(r)->seven_strength;
    return KR_SUCCESS;
}

static rl_result is_usb_full_key_enabled(rl_ipc_req *r)
{
    uint8_t *o = rl_ipc_out(r, 1);
    o[0] = ST(r)->usb_full_key;
    return KR_SUCCESS;
}

static rl_result enable_usb_full_key(rl_ipc_req *r)
{
    uint8_t v = 0;
    if (!rl_srv_in(r, &v, 1))
        return SF_RESULT_INVALID_HEADER;
    ST(r)->usb_full_key = v != 0;
    return KR_SUCCESS;
}

static rl_result acquire_palma_event(rl_ipc_req *r)
{
    return rl_ipc_push_copy(r, &ST(r)->palma_event->obj);
}

static rl_result palma_signal(rl_ipc_req *r)
{
    rl_hos_event_signal(ST(r)->palma_event);
    return KR_SUCCESS;
}

static rl_result set_comm_mode(rl_ipc_req *r)
{
    NEED(r, in_aruid_u64, in);
    ST(r)->comm_mode = in.v;
    return KR_SUCCESS;
}

static rl_result get_comm_mode(rl_ipc_req *r)
{
    uint64_t *o = rl_ipc_out(r, 8);
    *o = ST(r)->comm_mode;
    return KR_SUCCESS;
}

static const rl_ipc_cmd hid_cmds[] = {
    { 0, "CreateAppletResource", create_applet_resource },
    { 1, "ActivateDebugPad", activate_debug_pad },
    { 11, "ActivateTouchScreen", activate_touch_screen },
    { 21, "ActivateMouse", activate_mouse },
    { 26, "ActivateDebugMouse", activate_debug_mouse },
    { 31, "ActivateKeyboard", activate_keyboard },
    RL_SRV_STUB(32, "SendKeyboardLockKeyEvent"),
    { 40, "AcquireXpadIdEventHandle", acquire_xpad_id_event_handle },
    RL_SRV_STUB(41, "ReleaseXpadIdEventHandle"),
    RL_SRV_STUB(51, "ActivateXpad"),
    STUB_OUT(55, "GetXpadIds", out_zero8),
    RL_SRV_STUB(56, "ActivateJoyXpad"),
    RL_SRV_STUB(58, "GetJoyXpadLifoHandle"),
    STUB_OUT(59, "GetJoyXpadIds", out_zero8),
    RL_SRV_STUB(60, "ActivateSixAxisSensor"),
    RL_SRV_STUB(61, "DeactivateSixAxisSensor"),
    RL_SRV_STUB(62, "GetSixAxisSensorLifoHandle"),
    RL_SRV_STUB(63, "ActivateJoySixAxisSensor"),
    RL_SRV_STUB(64, "DeactivateJoySixAxisSensor"),
    RL_SRV_STUB(65, "GetJoySixAxisSensorLifoHandle"),
    RL_SRV_STUB(66, "StartSixAxisSensor"),
    RL_SRV_STUB(67, "StopSixAxisSensor"),
    { 68, "IsSixAxisSensorFusionEnabled", get_fusion_enabled },
    { 69, "EnableSixAxisSensorFusion", enable_fusion },
    { 70, "SetSixAxisSensorFusionParameters", set_fusion_params },
    { 71, "GetSixAxisSensorFusionParameters", get_fusion_params },
    { 72, "ResetSixAxisSensorFusionParameters", reset_fusion_params },
    { 73, "SetAccelerometerParameters", set_accel_params },
    { 74, "GetAccelerometerParameters", get_accel_params },
    { 75, "ResetAccelerometerParameters", reset_accel_params },
    { 76, "SetAccelerometerPlayMode", set_accel_play_mode },
    { 77, "GetAccelerometerPlayMode", get_accel_play_mode },
    { 78, "ResetAccelerometerPlayMode", reset_accel_play_mode },
    { 79, "SetGyroscopeZeroDriftMode", set_gyro_mode },
    { 80, "GetGyroscopeZeroDriftMode", get_gyro_mode },
    { 81, "ResetGyroscopeZeroDriftMode", reset_gyro_mode },
    STUB_OUT(82, "IsSixAxisSensorAtRest", out_true),
    STUB_OUT(83, "IsFirmwareUpdateAvailableForSixAxisSensor", out_false),
    { 84, "EnableSixAxisSensorUnalteredPassthrough", enable_unaltered_passthrough },
    { 85, "IsSixAxisSensorUnalteredPassthroughEnabled", get_unaltered_passthrough },
    RL_SRV_STUB(87, "LoadSixAxisSensorCalibrationParameter"),
    RL_SRV_STUB(88, "GetSixAxisSensorIcInformation"),
    RL_SRV_STUB(91, "ActivateGesture"),
    RL_SRV_STUB(92, "SetGestureOutputRanges"),
    { 100, "SetSupportedNpadStyleSet", set_supported_style_set },
    { 101, "GetSupportedNpadStyleSet", get_supported_style_set },
    { 102, "SetSupportedNpadIdType", set_supported_id_type },
    { 103, "ActivateNpad", activate_npad },
    RL_SRV_STUB(104, "DeactivateNpad"),
    { 106, "AcquireNpadStyleSetUpdateEventHandle", acquire_style_set_update_event },
    RL_SRV_STUB(107, "DisconnectNpad"),
    { 108, "GetPlayerLedPattern", get_player_led_pattern },
    { 109, "ActivateNpadWithRevision", activate_npad_with_revision },
    { 120, "SetNpadJoyHoldType", set_joy_hold_type },
    { 121, "GetNpadJoyHoldType", get_joy_hold_type },
    { 122, "SetNpadJoyAssignmentModeSingleByDefault", assign_single },
    { 123, "SetNpadJoyAssignmentModeSingle", assign_single },
    { 124, "SetNpadJoyAssignmentModeDual", assign_dual },
    RL_SRV_STUB(125, "MergeSingleJoyAsDualJoy"),
    RL_SRV_STUB(126, "StartLrAssignmentMode"),
    RL_SRV_STUB(127, "StopLrAssignmentMode"),
    { 128, "SetNpadHandheldActivationMode", set_handheld_activation_mode },
    { 129, "GetNpadHandheldActivationMode", get_handheld_activation_mode },
    RL_SRV_STUB(130, "SwapNpadAssignment"),
    { 131, "IsUnintendedHomeButtonInputProtectionEnabled", get_home_protection },
    { 132, "EnableUnintendedHomeButtonInputProtection", set_home_protection },
    { 133, "SetNpadJoyAssignmentModeSingleWithDestination", assign_single_with_destination },
    { 134, "SetNpadAnalogStickUseCenterClamp", set_center_clamp },
    { 200, "GetVibrationDeviceInfo", get_vibration_device_info },
    { 201, "SendVibrationValue", send_vibration_value },
    { 202, "GetActualVibrationValue", get_actual_vibration_value },
    { 203, "CreateActiveVibrationDeviceList", create_active_vibration_device_list },
    { 204, "PermitVibration", permit_vibration },
    { 205, "IsVibrationPermitted", is_vibration_permitted },
    { 206, "SendVibrationValues", send_vibration_values },
    RL_SRV_STUB(207, "SendVibrationGcErmCommand"),
    STUB_OUT(208, "GetActualVibrationGcErmCommand", out_zero8),
    RL_SRV_STUB(209, "BeginPermitVibrationSession"),
    RL_SRV_STUB(210, "EndPermitVibrationSession"),
    STUB_OUT(211, "IsVibrationDeviceMounted", out_true),
    RL_SRV_STUB(300, "ActivateConsoleSixAxisSensor"),
    RL_SRV_STUB(301, "StartConsoleSixAxisSensor"),
    RL_SRV_STUB(302, "StopConsoleSixAxisSensor"),
    RL_SRV_STUB(303, "ActivateSevenSixAxisSensor"),
    RL_SRV_STUB(304, "StartSevenSixAxisSensor"),
    RL_SRV_STUB(305, "StopSevenSixAxisSensor"),
    RL_SRV_STUB(306, "InitializeSevenSixAxisSensor"),
    RL_SRV_STUB(307, "FinalizeSevenSixAxisSensor"),
    { 308, "SetSevenSixAxisSensorFusionStrength", set_seven_strength },
    { 309, "GetSevenSixAxisSensorFusionStrength", get_seven_strength },
    RL_SRV_STUB(310, "ResetSevenSixAxisSensorTimestamp"),
    { 400, "IsUsbFullKeyControllerEnabled", is_usb_full_key_enabled },
    { 401, "EnableUsbFullKeyController", enable_usb_full_key },
    STUB_OUT(402, "IsUsbFullKeyControllerConnected", out_true),
    STUB_OUT(403, "HasBattery", out_true),
    STUB_OUT(404, "HasLeftRightBattery", out_two_true),
    STUB_OUT(405, "GetNpadInterfaceType", out_zero1),
    STUB_OUT(406, "GetNpadLeftRightInterfaceType", out_zero2),
    STUB_OUT(500, "GetPalmaConnectionHandle", out_zero8),
    { 501, "InitializePalma", palma_signal },
    { 502, "AcquirePalmaOperationCompleteEvent", acquire_palma_event },
    STUB_OUT(503, "GetPalmaOperationInfo", out_zero8),
    { 504, "PlayPalmaActivity", palma_signal },
    { 505, "SetPalmaFrModeType", palma_signal },
    RL_SRV_STUB(506, "ReadPalmaStep"),
    { 507, "EnablePalmaStep", palma_signal },
    { 508, "ResetPalmaStep", palma_signal },
    RL_SRV_STUB(509, "ReadPalmaApplicationSection"),
    { 510, "WritePalmaApplicationSection", palma_signal },
    RL_SRV_STUB(511, "ReadPalmaUniqueCode"),
    RL_SRV_STUB(512, "SetPalmaUniqueCodeInvalid"),
    RL_SRV_STUB(522, "SetIsPalmaAllConnectable"),
    RL_SRV_STUB(525, "SetPalmaBoostMode"),
    { 1000, "SetNpadCommunicationMode", set_comm_mode },
    { 1001, "GetNpadCommunicationMode", get_comm_mode },
    RL_SRV_STUB(1002, "SetTouchScreenConfiguration"),
    RL_SRV_STUB(1004, "SetTouchScreenResolution"),
};

static void hid_destroy(void *state)
{
    hid_srv *s = state;
    hos_kobj_unref(&s->xpad_event->obj);
    hos_kobj_unref(&s->palma_event->obj);
    free(s);
}

static const rl_ipc_class hid_cls = {
    "hid", hid_cmds, (uint32_t)(sizeof hid_cmds / sizeof hid_cmds[0]), HID_POINTER_BUFFER_SIZE, hid_destroy
};

static rl_ipc_object *hid_factory(void)
{
    hid_srv *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->xpad_event = rl_hos_event_create();
    s->palma_event = rl_hos_event_create();
    if (!s->xpad_event || !s->palma_event) {
        if (s->xpad_event)
            hos_kobj_unref(&s->xpad_event->obj);
        if (s->palma_event)
            hos_kobj_unref(&s->palma_event->obj);
        free(s);
        return NULL;
    }
    rl_hos_event_signal(s->xpad_event);
    s->vibration_permitted = true;
    s->gyro_mode = 1;
    return rl_srv_object(&hid_cls, s);
}

void rl_srv_install_hid(void)
{
    hid_core_reset();
    rl_ipc_register_service("hid", hid_factory);
    hid_sys_install();
}
