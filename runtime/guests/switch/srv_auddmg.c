/**
 * @file srv_auddmg.c
 * @brief The Switch 2 audio device manager auddmg:u
 * (nn::audio::devmgr::IUserSessionManager and IUserSession).
 *
 * switchbrew lists the command ids without names or layouts. The names and
 * layouts here are verified from a Switch 2 SDK's client: the CmifProxy of
 * IUserSession lists its methods in id order, each detail::*Impl calls one
 * method, and the proxy writes the raw data read here. It reports one
 * virtual input device, as on the console, whose microphone is always there,
 * taking any stream format (inferred from Longshot, host/nn_audio_in.c, a
 * native recompilation read for behavior only). Its name, "MainAudioIn", is
 * the SDK's own name for the main audio input: entry 5 of the stream source
 * name table at sdk+0xc0ca18 that
 * nn::audio::capture::detail::SystemAudioCaptureServerImplBase::
 * ProcessSystemAudioMonitorServiceForInputStream (sdk+0x27ab2c) indexes,
 * beside "RawAudioIn" and "TrueRawAudioIn" (verified in a Switch 2 SDK;
 * that the device list carries the same name is inferred, and Longshot
 * agrees). Lichii (src/core/hle/service/audio/nx2_audio_device_manager.h)
 * agrees on the command ids; its device name "AudioVirtualInput0" appears
 * nowhere in the SDK or the program. Commands relift does not answer yet
 * stop by name.
 */
#include "hos.h"
#include "hos_audio.h"
#include "services.h"

#include <string.h>

/** @brief DeviceFunctionality of input devices: the value nn::audio::ListVirtualInputDevices passes. */
#define DEVMGR_INPUT 2u
#define DEVMGR_NAME_BYTES 0x100u

static const char main_input[] = "MainAudioIn";

/** @brief True for the input functionality; a named stop for any other, which relift does not answer yet. */
static bool input_only(rl_ipc_req *r, const char *cmd, uint32_t f)
{
    if (f == DEVMGR_INPUT)
        return true;
    rl_ipc_stop(r, "auddmg", "IUserSession %s for device functionality %u is not answered yet", cmd, f);
    return false;
}

/**
 * @brief 0 ListVirtualDeviceNames: a bool and a DeviceFunctionality in, the
 * names into a type-6 buffer of 0x100-byte entries; the count out.
 */
static rl_result list_virtual_device_names(rl_ipc_req *r)
{
    struct {
        uint8_t flag, pad[3];
        uint32_t functionality;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (!input_only(r, "ListVirtualDeviceNames", in.functionality))
        return AUDIO_OPERATION_FAILED;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    int32_t n = 0;
    if (out.size >= DEVMGR_NAME_BYTES) {
        uint8_t name[DEVMGR_NAME_BYTES] = { 0 };
        memcpy(name, main_input, sizeof main_input - 1);
        if (!rl_ipc_buf_write(&out, name, sizeof name))
            return SF_RESULT_INVALID_HEADER;
        n = 1;
    }
    int32_t *o = rl_ipc_out(r, 4);
    if (!o)
        return SF_RESULT_INVALID_HEADER;
    *o = n;
    return KR_SUCCESS;
}

/** @brief 100 GetDefaultVirtualDeviceName: a DeviceFunctionality in, the name into a type-6 buffer. */
static rl_result get_default_virtual_device_name(rl_ipc_req *r)
{
    uint32_t f;
    if (!rl_srv_in(r, &f, 4))
        return SF_RESULT_INVALID_HEADER;
    if (!input_only(r, "GetDefaultVirtualDeviceName", f))
        return AUDIO_OPERATION_FAILED;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint8_t name[DEVMGR_NAME_BYTES] = { 0 };
    memcpy(name, main_input, sizeof main_input - 1);
    if (out.size < DEVMGR_NAME_BYTES || !rl_ipc_buf_write(&out, name, sizeof name))
        return SF_RESULT_INVALID_HEADER;
    return KR_SUCCESS;
}

/**
 * @brief 300 IsVirtualDeviceStreamFormatSupported: the name in a type-5
 * buffer, a 12-byte StreamFormat and a DeviceFunctionality in; a bool out,
 * true for the input device whatever the format.
 */
static rl_result is_virtual_device_stream_format_supported(rl_ipc_req *r)
{
    struct {
        uint8_t format[12];
        uint32_t functionality;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (!input_only(r, "IsVirtualDeviceStreamFormatSupported", in.functionality))
        return AUDIO_OPERATION_FAILED;
    rl_ipc_buf nb = rl_ipc_auto_in(r, 0);
    char name[DEVMGR_NAME_BYTES] = { 0 };
    if (!rl_ipc_buf_read(&nb, name, nb.size < sizeof name ? nb.size : sizeof name))
        return SF_RESULT_INVALID_HEADER;
    name[sizeof name - 1] = 0;
    uint8_t *o = rl_ipc_out(r, 1);
    if (!o)
        return SF_RESULT_INVALID_HEADER;
    *o = strcmp(name, main_input) == 0;
    return KR_SUCCESS;
}

/**
 * @brief 401 GetVirtualDeviceProfile: the device's name in a type-5 buffer
 * and a DeviceFunctionality in; a 0x228-byte VirtualDeviceProfile into a
 * type-6 buffer. Verified from the SDK's InputStreamImpl::Open, which reads
 * two fields: the u64 at 0x218, a latency in nanoseconds the stream's frame
 * count must cover at its rate, and the u32 at 0x220, the device value it
 * passes to sysmix CreateStream (2 for input, which audsmx:u takes). The
 * values, and the rest of the block (0x100: 3, 0x104: 2, 0x108: channels,
 * 0x10c: rate, 0x110: the physical device's name), follow Lichii
 * (src/core/hle/service/audio/nx2_audio_device_manager.h): 10 ms latency,
 * 2 channels, 48000 Hz, "AudioBuiltInMicrophone" (inferred).
 */
static rl_result get_virtual_device_profile(rl_ipc_req *r)
{
    uint32_t f;
    if (!rl_srv_in(r, &f, 4))
        return SF_RESULT_INVALID_HEADER;
    if (!input_only(r, "GetVirtualDeviceProfile", f))
        return AUDIO_OPERATION_FAILED;
    uint8_t block[0x228] = { 0 };
    static const char physical[] = "AudioBuiltInMicrophone";
    const uint32_t words[4] = { 3, 2, 2, 48000 };
    const uint64_t latency_ns = 10000000;
    const uint32_t device = 2;
    memcpy(block, main_input, sizeof main_input - 1);
    memcpy(block + 0x100, words, sizeof words);
    memcpy(block + 0x110, physical, sizeof physical - 1);
    memcpy(block + 0x218, &latency_ns, 8);
    memcpy(block + 0x220, &device, 4);
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    if (out.size < sizeof block || !rl_ipc_buf_write(&out, block, sizeof block))
        return SF_RESULT_INVALID_HEADER;
    return KR_SUCCESS;
}

/** @brief A command of IUserSession relift does not answer yet: stops by name. */
static rl_result not_answered(rl_ipc_req *r)
{
    static const struct {
        uint32_t id;
        const char *name;
    } names[] = {
        { 200, "IsVirtualDeviceAvailable" },
        { 500, "AcquireVirtualDeviceStateChangeEvent" },
        { 600, "ReleaseVirtualDeviceStateChangeEvent" },
        { 700, "AttachStream" },
        { 800, "DetachStream" },
        { 900, "GetPhysicalDeviceName" },
        { 1000, "GetDeviceStreamFormat" },
        { 1500, "GetMixCount" },
        { 1550, "GetMixTopology" },
        { 1600, "GetMixNode" },
        { 2000, "IsCaptureDeviceStreamFormatSupported" },
        { 2100, "AttachCaptureStream" },
        { 2301, "GetCaptureDeviceProfile" },
        { 3000, "AttachInjectionStream" },
    };
    const char *name = "?";
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].id == r->cmd)
            name = names[i].name;
    rl_ipc_stop(r, "auddmg", "IUserSession %u %s is not answered yet", r->cmd, name);
    return AUDIO_OPERATION_FAILED;
}

static const rl_ipc_cmd session_cmds[] = {
    { 0, "ListVirtualDeviceNames", list_virtual_device_names },
    { 100, "GetDefaultVirtualDeviceName", get_default_virtual_device_name },
    { 200, "IsVirtualDeviceAvailable", not_answered },
    { 300, "IsVirtualDeviceStreamFormatSupported", is_virtual_device_stream_format_supported },
    { 401, "GetVirtualDeviceProfile", get_virtual_device_profile },
    { 500, "AcquireVirtualDeviceStateChangeEvent", not_answered },
    { 600, "ReleaseVirtualDeviceStateChangeEvent", not_answered },
    { 700, "AttachStream", not_answered },
    { 800, "DetachStream", not_answered },
    { 900, "GetPhysicalDeviceName", not_answered },
    { 1000, "GetDeviceStreamFormat", not_answered },
    { 1500, "GetMixCount", not_answered },
    { 1550, "GetMixTopology", not_answered },
    { 1600, "GetMixNode", not_answered },
    { 2000, "IsCaptureDeviceStreamFormatSupported", not_answered },
    { 2100, "AttachCaptureStream", not_answered },
    { 2301, "GetCaptureDeviceProfile", not_answered },
    { 3000, "AttachInjectionStream", not_answered },
};

static const rl_ipc_class session_class = { "nn::audio::devmgr::IUserSession", session_cmds,
                                            sizeof session_cmds / sizeof session_cmds[0], 0, NULL };

/** @brief 0: the client's pid, a u32 (2 from the SDK) and its AppletResourceUserId in; an IUserSession out. */
static rl_result create_user_session(rl_ipc_req *r)
{
    struct {
        uint32_t kind, pad;
        uint64_t aruid;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    rl_ipc_object *o = rl_srv_object(&session_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "CreateUserSession", create_user_session },
};

static const rl_ipc_class manager_class = { "nn::audio::devmgr::IUserSessionManager", manager_cmds,
                                            sizeof manager_cmds / sizeof manager_cmds[0], 0, NULL };

static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_class, NULL); }

void rl_srv_install_auddmg(void)
{
    if (hos_switch2())
        rl_ipc_register_service("auddmg:u", manager_factory);
}
