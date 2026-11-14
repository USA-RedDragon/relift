/**
 * @file srv_set.c
 * @brief The settings services: set, set:sys, set:cal and set:fd.
 *
 * Command ids, result codes and the language list follow Ryujinx (MIT)
 * Services/Settings and switchbrew.org; the structure sizes and the command
 * ids of the setsys settings follow libnx (ISC) set.h and set.c. Settings
 * that a program can write and read back are plain byte slots in a table;
 * their values live for the run.
 */
#include "services.h"

#include <stdio.h>
#include <string.h>

/** @brief The one firmware version the services report: major, minor, micro. */
static const uint8_t k_firmware[3] = { 19, 0, 0 };

#define SET_RESULT(desc) RL_MAKE_RESULT(105, desc)
#define SET_NULL_BUFFER_NICKNAME SET_RESULT(808)
#define SET_LANGUAGE_OUT_OF_RANGE SET_RESULT(625)

#define SLOT_MAX 64
#define SETTINGS_NAME_MAX 0x40u
#define SET_REGION_USA 1u
#define SET_PLATFORM_REGION_GLOBAL 1u
#define SET_PRODUCT_MODEL_NX 1u
#define PROGRAM_ID_QLAUNCH 0x0100000000001000ull
#define PROGRAM_ID_OVERLAY 0x010000000000100Cull

static const char *const k_language_codes[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419",
    "zh-Hans", "zh-Hant", "pt-BR",
};
#define N_LANGUAGES ((uint32_t)(sizeof k_language_codes / sizeof k_language_codes[0]))
#define LEGACY_LANGUAGE_MAX 0xFu
#define DEFAULT_LANGUAGE 1u

#define SLOT_LIST(X, G)                                                                                        \
    X(7, 8, 1, "LockScreenFlag")                                                                               \
    X(25, 26, 1, "ConsoleInformationUploadFlag")                                                               \
    X(27, 28, 1, "AutomaticApplicationDownloadFlag")                                                           \
    X(45, 46, 1, "SpeakerAutoMuteFlag")                                                                        \
    X(47, 48, 1, "QuestFlag")                                                                                  \
    X(60, 61, 1, "UserSystemClockAutomaticCorrectionEnabled")                                                  \
    X(65, 66, 1, "Usb30EnableFlag")                                                                            \
    X(69, 70, 1, "NfcEnableFlag")                                                                              \
    X(73, 74, 1, "WirelessLanEnableFlag")                                                                      \
    X(88, 89, 1, "BluetoothEnableFlag")                                                                        \
    X(95, 96, 1, "AutoUpdateEnableFlag")                                                                       \
    X(99, 100, 1, "BatteryPercentageFlag")                                                                     \
    X(101, 102, 1, "ExternalRtcResetFlag")                                                                     \
    X(103, 104, 1, "UsbFullKeyEnableFlag")                                                                     \
    X(111, 112, 1, "BluetoothAfhEnableFlag")                                                                   \
    X(113, 114, 1, "BluetoothBoostEnableFlag")                                                                 \
    X(115, 116, 1, "InRepairProcessEnableFlag")                                                                \
    X(117, 118, 1, "HeadphoneVolumeUpdateFlag")                                                                \
    X(141, 142, 1, "RequiresRunRepairTimeReviser")                                                             \
    X(156, 157, 1, "PctlReadyFlag")                                                                            \
    X(164, 165, 1, "Usb30HostEnableFlag")                                                                      \
    X(166, 167, 1, "Usb30DeviceEnableFlag")                                                                    \
    X(179, 180, 1, "ZoomFlag")                                                                                 \
        X(201, 202, 1, "FieldTestingFlag")                                                                         \
    G(62, 1, "DebugModeFlag")                                                                                  \
    G(138, 1, "WebInspectorFlag")                                                                              \
    G(186, 1, "MemoryUsageRateFlag")                                                                           \
    X(23, 24, 4, "ColorSetId")                                                                                 \
    X(63, 64, 4, "PrimaryAlbumStorage")                                                                        \
    X(80, 81, 4, "LdnChannel")                                                                                 \
    X(109, 110, 4, "HeadphoneVolumeWarningCount")                                                              \
    X(120, 121, 4, "PushNotificationActivityModeOnSleep")                                                      \
    X(122, 123, 4, "ServiceDiscoveryControlSettings")                                                          \
    X(124, 125, 4, "ErrorReportSharePermission")                                                               \
    X(126, 127, 4, "AppletLaunchFlags")                                                                        \
    X(136, 137, 4, "KeyboardLayout")                                                                           \
    X(170, 171, 4, "ChineseTraditionalInputMethod")                                                            \
    X(172, 173, 4, "PtmCycleCountReliability")                                                                 \
    X(183, 184, 4, "PlatformRegion")                                                                           \
    X(187, 188, 4, "TouchScreenMode")                                                                          \
    G(79, 4, "ProductModel")                                                                                   \
    G(185, 4, "HomeMenuSchemeModel")                                                                           \
    X(92, 91, 8, "ShutdownRtcValue")                                                                           \
    X(106, 105, 8, "ExternalSteadyClockInternalOffset")                                                        \
    G(51, 8, "InitialSystemAppletProgramId")                                                                   \
    G(52, 8, "OverlayDispProgramId")                                                                           \
    G(55, 8, "WirelessCertificationFileSize")                                                                  \
    X(162, 163, 1, "PtmBatteryVersion")                                                                        \
    X(35, 36, 4, "VibrationMasterVolume")                                                                      \
    X(9, 10, 40, "BacklightSettings")                                                                          \
    X(107, 108, 44, "BacklightSettingsEx")                                                                     \
    X(17, 18, 4, "AccountSettings")                                                                            \
    X(29, 30, 24, "NotificationSettings")                                                                      \
    X(39, 40, 32, "TvSettings")                                                                                \
    X(49, 50, 8, "DataDeletionSettings")                                                                       \
    X(71, 72, 12, "SleepSettings")                                                                             \
    X(75, 76, 32, "InitialLaunchSettings")                                                                     \
    X(84, 85, 24, "PtmBatteryLot")                                                                             \
    X(86, 87, 24, "PtmFuelGaugeParameter")                                                                     \
    X(128, 129, 12, "ConsoleSixAxisSensorAccelerationBias")                                                    \
    X(130, 131, 12, "ConsoleSixAxisSensorAngularVelocityBias")                                                 \
    X(132, 133, 36, "ConsoleSixAxisSensorAccelerationGain")                                                    \
    X(134, 135, 36, "ConsoleSixAxisSensorAngularVelocityGain")                                                 \
    X(145, 146, 12, "ConsoleSixAxisSensorAngularVelocityTimeBias")                                             \
    X(147, 148, 36, "ConsoleSixAxisSensorAngularAcceleration")                                                 \
    X(158, 159, 16, "AnalogStickUserCalibrationL")                                                             \
    X(160, 161, 16, "AnalogStickUserCalibrationR")                                                             \
    X(175, 176, 8, "ThemeSettings")                                                                            \
    X(177, 178, 16, "ThemeKey")                                                                                \
    X(13, 14, 16, "ExternalSteadyClockSourceId")                                                               \
    X(15, 16, 32, "UserSystemClockContext")                                                                    \
    X(58, 59, 32, "NetworkSystemClockContext")                                                                 \
    X(53, 54, 0x24, "DeviceTimeZoneLocationName")                                                              \
    X(150, 151, 24, "DeviceTimeZoneLocationUpdatedTime")                                                       \
    X(152, 153, 24, "UserSystemClockAutomaticCorrectionUpdatedTime")                                           \
    G(67, 24, "BatteryLot")                                                                                    \
    G(149, 64, "RebootlessSystemUpdateVersion")                                                                \
    G(174, 20, "HomeMenuScheme")                                                                               \
    G(90, 16, "MiiAuthorId")

typedef struct slot_def {
    uint16_t get, set;
    uint8_t size;
} slot_def;

#define DEF_RW(g, s, n, nm) { (g), (s), (n) },
#define DEF_G(g, n, nm) { (g), 0, (n) },
static const slot_def k_slots[] = { SLOT_LIST(DEF_RW, DEF_G) };
#define N_SLOTS ((uint32_t)(sizeof k_slots / sizeof k_slots[0]))

#define AUDIO_DEVICES 3u
#define AUDIO_TARGETS 4u
#define AUDIO_MODE_DEFAULT 1u

static struct {
    uint8_t slot[N_SLOTS][SLOT_MAX];
    uint64_t language;
    uint32_t region;
    uint8_t audio_volume[AUDIO_DEVICES][8];
    uint32_t audio_mode[AUDIO_TARGETS];
    uint8_t edid[512];
    char nickname[0x80];
    hos_revent *telemetry_event, *fatal_event;
} g_set;

static uint64_t language_code(uint32_t index)
{
    uint64_t code = 0;
    const char *s = k_language_codes[index];
    for (uint32_t i = 0; s[i]; i++)
        code |= (uint64_t)(uint8_t)s[i] << (8 * i);
    return code;
}

static uint8_t *slot_by_get(uint32_t get)
{
    for (uint32_t i = 0; i < N_SLOTS; i++)
        if (k_slots[i].get == get)
            return g_set.slot[i];
    return NULL;
}

static void slot_init(uint32_t get, const void *value, uint32_t size)
{
    uint8_t *p = slot_by_get(get);
    if (p)
        memcpy(p, value, size);
}

static void set_reset(void)
{
    hos_revent *ev[2] = { g_set.telemetry_event, g_set.fatal_event };
    for (int i = 0; i < 2; i++)
        if (ev[i])
            hos_kobj_unref(&ev[i]->obj);
    memset(&g_set, 0, sizeof g_set);
    g_set.language = language_code(DEFAULT_LANGUAGE);
    g_set.region = SET_REGION_USA;
    for (uint32_t i = 0; i < AUDIO_TARGETS; i++)
        g_set.audio_mode[i] = AUDIO_MODE_DEFAULT;
    snprintf(g_set.nickname, sizeof g_set.nickname, "relift");

    uint8_t yes = 1;
    uint32_t u32 = SET_PLATFORM_REGION_GLOBAL;
    uint64_t u64 = PROGRAM_ID_QLAUNCH;
    float volume = 1.0f;
    slot_init(60, &yes, 1);
    slot_init(183, &u32, 4);
    u32 = SET_PRODUCT_MODEL_NX;
    slot_init(79, &u32, 4);
    slot_init(51, &u64, 8);
    u64 = PROGRAM_ID_OVERLAY;
    slot_init(52, &u64, 8);
    slot_init(35, &volume, 4);
    slot_init(53, "UTC", 4);
    static const uint8_t mii_author[16] = { 'r', 'e', 'l', 'i', 'f', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    slot_init(90, mii_author, 16);
}

static rl_result slot_get(rl_ipc_req *r)
{
    for (uint32_t i = 0; i < N_SLOTS; i++)
        if (k_slots[i].get == r->cmd) {
            memcpy(rl_ipc_out(r, k_slots[i].size), g_set.slot[i], k_slots[i].size);
            return KR_SUCCESS;
        }
    return SF_RESULT_UNKNOWN_COMMAND;
}

static rl_result slot_set(rl_ipc_req *r)
{
    for (uint32_t i = 0; i < N_SLOTS; i++)
        if (k_slots[i].set == r->cmd) {
            uint8_t tmp[SLOT_MAX];
            if (!rl_srv_in(r, tmp, k_slots[i].size))
                return SF_RESULT_INVALID_HEADER;
            memcpy(g_set.slot[i], tmp, k_slots[i].size);
            return KR_SUCCESS;
        }
    return SF_RESULT_UNKNOWN_COMMAND;
}

static rl_result copy_out(rl_ipc_req *r, const void *src, uint32_t size)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    return rl_ipc_buf_write(&b, src, size) ? KR_SUCCESS : KR_INVALID_MEM_STATE;
}

static rl_result set_get_language_code(rl_ipc_req *r)
{
    *(uint64_t *)rl_ipc_out(r, 8) = g_set.language;
    return KR_SUCCESS;
}

static rl_result available_codes(rl_ipc_req *r, uint32_t max)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    uint32_t n = (uint32_t)(b.size / 8);
    if (n > max)
        n = max;
    uint64_t codes[N_LANGUAGES];
    for (uint32_t i = 0; i < n; i++)
        codes[i] = language_code(i);
    if (n && !rl_ipc_buf_write(&b, codes, (uint64_t)n * 8))
        return KR_INVALID_MEM_STATE;
    *(uint32_t *)rl_ipc_out(r, 4) = n;
    return KR_SUCCESS;
}

static rl_result set_get_available_language_codes(rl_ipc_req *r) { return available_codes(r, LEGACY_LANGUAGE_MAX); }

static rl_result set_get_available_language_codes2(rl_ipc_req *r) { return available_codes(r, N_LANGUAGES); }

static rl_result set_make_language_code(rl_ipc_req *r)
{
    int32_t index;
    if (!rl_srv_in(r, &index, 4))
        return SF_RESULT_INVALID_HEADER;
    if (index < 0 || (uint32_t)index >= N_LANGUAGES)
        return SET_LANGUAGE_OUT_OF_RANGE;
    *(uint64_t *)rl_ipc_out(r, 8) = language_code((uint32_t)index);
    return KR_SUCCESS;
}

static rl_result set_get_language_count(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = N_LANGUAGES < LEGACY_LANGUAGE_MAX ? N_LANGUAGES : LEGACY_LANGUAGE_MAX;
    return KR_SUCCESS;
}

static rl_result set_get_language_count2(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = N_LANGUAGES;
    return KR_SUCCESS;
}

static rl_result set_get_region_code(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = g_set.region;
    return KR_SUCCESS;
}

static rl_result set_get_quest_flag(rl_ipc_req *r)
{
    *(uint8_t *)rl_ipc_out(r, 1) = slot_by_get(47)[0];
    return KR_SUCCESS;
}

static rl_result get_nickname(rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_out(r, 0);
    if (!b.addr)
        return SET_NULL_BUFFER_NICKNAME;
    return rl_ipc_buf_write(&b, g_set.nickname, sizeof g_set.nickname) ? KR_SUCCESS : KR_INVALID_MEM_STATE;
}

static rl_result sys_set_nickname(rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    char tmp[sizeof g_set.nickname] = { 0 };
    if (!b.addr || !rl_ipc_buf_read(&b, tmp, b.size < sizeof tmp ? b.size : sizeof tmp))
        return KR_INVALID_MEM_STATE;
    tmp[sizeof tmp - 1] = 0;
    memcpy(g_set.nickname, tmp, sizeof tmp);
    return KR_SUCCESS;
}

static const rl_ipc_cmd set_cmds[] = {
    { 0, "GetLanguageCode", set_get_language_code },
    { 1, "GetAvailableLanguageCodes", set_get_available_language_codes },
    { 2, "MakeLanguageCode", set_make_language_code },
    { 3, "GetAvailableLanguageCodeCount", set_get_language_count },
    { 4, "GetRegionCode", set_get_region_code },
    { 5, "GetAvailableLanguageCodes2", set_get_available_language_codes2 },
    { 6, "GetAvailableLanguageCodeCount2", set_get_language_count2 },
    { 8, "GetQuestFlag", set_get_quest_flag },
    { 11, "GetDeviceNickName", get_nickname },
};

static const rl_ipc_class set_class = { "set", set_cmds, sizeof set_cmds / sizeof set_cmds[0], 0x1000, NULL };

static rl_ipc_object *set_factory(void) { return rl_srv_object(&set_class, NULL); }

static rl_result sys_set_language_code(rl_ipc_req *r)
{
    uint64_t v;
    if (!rl_srv_in(r, &v, 8))
        return SF_RESULT_INVALID_HEADER;
    g_set.language = v;
    return KR_SUCCESS;
}

static rl_result sys_set_region_code(rl_ipc_req *r)
{
    uint32_t v;
    if (!rl_srv_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    g_set.region = v;
    return KR_SUCCESS;
}

static rl_result sys_firmware_version(rl_ipc_req *r)
{
    uint8_t v[0x100] = { 0 };
    v[0] = k_firmware[0];
    v[1] = k_firmware[1];
    v[2] = k_firmware[2];
    v[4] = 1;
    memcpy(v + 8, "NX", 3);
    memcpy(v + 0x28, "0000000000000000000000000000000000000000", 40);
    snprintf((char *)v + 0x68, 0x18, "%u.%u.%u", k_firmware[0], k_firmware[1], k_firmware[2]);
    snprintf((char *)v + 0x80, 0x80, "NintendoSDK Firmware for NX %u.%u.%u-1.0", k_firmware[0], k_firmware[1],
             k_firmware[2]);
    return copy_out(r, v, sizeof v);
}

static rl_result sys_firmware_digest(rl_ipc_req *r)
{
    memcpy(rl_ipc_out(r, 0x40), "relift", 6);
    return KR_SUCCESS;
}

static rl_result sys_serial_number(rl_ipc_req *r)
{
    memcpy(rl_ipc_out(r, 0x18), "RLF00000000000", 14);
    return KR_SUCCESS;
}

static rl_result sys_get_edid(rl_ipc_req *r) { return copy_out(r, g_set.edid, sizeof g_set.edid); }

static rl_result sys_set_edid(rl_ipc_req *r)
{
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    if (b.size != sizeof g_set.edid || !rl_ipc_buf_read(&b, g_set.edid, sizeof g_set.edid))
        return KR_INVALID_MEM_STATE;
    return KR_SUCCESS;
}

static rl_result sys_get_host_fs_mount_point(rl_ipc_req *r)
{
    uint8_t zero[256] = { 0 };
    return copy_out(r, zero, sizeof zero);
}

static rl_result sys_get_audio_volume(rl_ipc_req *r)
{
    uint32_t device;
    if (!rl_srv_in(r, &device, 4))
        return SF_RESULT_INVALID_HEADER;
    if (device >= AUDIO_DEVICES)
        return SET_RESULT(1);
    memcpy(rl_ipc_out(r, 8), g_set.audio_volume[device], 8);
    return KR_SUCCESS;
}

static rl_result sys_set_audio_volume(rl_ipc_req *r)
{
    uint8_t in[12];
    uint32_t device;
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    memcpy(&device, in + 8, 4);
    if (device >= AUDIO_DEVICES)
        return SET_RESULT(1);
    memcpy(g_set.audio_volume[device], in, 8);
    return KR_SUCCESS;
}

static rl_result sys_get_audio_output_mode(rl_ipc_req *r)
{
    uint32_t target;
    if (!rl_srv_in(r, &target, 4))
        return SF_RESULT_INVALID_HEADER;
    if (target >= AUDIO_TARGETS)
        return SET_RESULT(687);
    *(uint32_t *)rl_ipc_out(r, 4) = g_set.audio_mode[target];
    return KR_SUCCESS;
}

static rl_result sys_set_audio_output_mode(rl_ipc_req *r)
{
    uint32_t in[2];
    if (!rl_srv_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (in[0] >= AUDIO_TARGETS)
        return SET_RESULT(687);
    g_set.audio_mode[in[0]] = in[1];
    return KR_SUCCESS;
}

static rl_result dirty_event(rl_ipc_req *r, hos_revent **slot)
{
    if (!*slot)
        *slot = rl_hos_event_create();
    if (!*slot)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &(*slot)->obj);
}

static rl_result sys_telemetry_event(rl_ipc_req *r) { return dirty_event(r, &g_set.telemetry_event); }

static rl_result sys_fatal_event(rl_ipc_req *r) { return dirty_event(r, &g_set.fatal_event); }

static rl_result sys_dirty_flags(rl_ipc_req *r)
{
    (void)rl_ipc_out(r, 16);
    return KR_SUCCESS;
}

typedef struct item {
    const char *key;
    enum { ITEM_BOOL, ITEM_INT } kind;
    int32_t value;
} item;

static const item k_items[] = {
    { "nifm!is_communication_control_enabled_for_test", ITEM_BOOL, 0 },
    { "settings_debug!is_debug_mode_enabled", ITEM_BOOL, 0 },
    { "time!standard_steady_clock_test_offset_minutes", ITEM_INT, 0 },
    { "time!standard_steady_clock_rtc_update_interval_minutes", ITEM_INT, 5 },
    { "time!standard_network_clock_sufficient_accuracy_minutes", ITEM_INT, 43200 },
    { "time!standard_user_clock_initial_year", ITEM_INT, 2019 },
};

static const item *item_find(rl_ipc_req *r)
{
    char name[SETTINGS_NAME_MAX + 1] = { 0 }, key[SETTINGS_NAME_MAX + 1] = { 0 }, full[2 * SETTINGS_NAME_MAX + 2];
    rl_ipc_buf n = rl_ipc_auto_in(r, 0), k = rl_ipc_auto_in(r, 1);
    if (!n.addr || !k.addr || n.size > SETTINGS_NAME_MAX || k.size > SETTINGS_NAME_MAX)
        return NULL;
    if (!rl_ipc_buf_read(&n, name, n.size) || !rl_ipc_buf_read(&k, key, k.size))
        return NULL;
    snprintf(full, sizeof full, "%s!%s", name, key);
    for (uint32_t i = 0; i < sizeof k_items / sizeof k_items[0]; i++)
        if (strcmp(k_items[i].key, full) == 0)
            return &k_items[i];
    return NULL;
}

static uint32_t item_size(const item *it) { return it->kind == ITEM_BOOL ? 1 : 4; }

static rl_result sys_item_value_size(rl_ipc_req *r)
{
    const item *it = item_find(r);
    *(uint64_t *)rl_ipc_out(r, 8) = it ? item_size(it) : 0;
    return KR_SUCCESS;
}

static rl_result sys_item_value(rl_ipc_req *r)
{
    const item *it = item_find(r);
    uint64_t size = 0;
    if (it) {
        rl_ipc_buf b = rl_ipc_auto_out(r, 0);
        uint8_t v[4];
        size = item_size(it);
        memcpy(v, &it->value, 4);
        if (!rl_ipc_buf_write(&b, v, size))
            return KR_INVALID_MEM_STATE;
    }
    *(uint64_t *)rl_ipc_out(r, 8) = size;
    return KR_SUCCESS;
}

#define CMD_RW(g, s, n, nm) { (g), "Get" nm, slot_get }, { (s), "Set" nm, slot_set },
#define CMD_G(g, n, nm) { (g), "Get" nm, slot_get },

static const rl_ipc_cmd sys_cmds[] = {
    SLOT_LIST(CMD_RW, CMD_G){ 0, "SetLanguageCode", sys_set_language_code },
    { 3, "GetFirmwareVersion", sys_firmware_version },
    { 4, "GetFirmwareVersion2", sys_firmware_version },
    { 5, "GetFirmwareVersionDigest", sys_firmware_digest },
    { 19, "GetAudioVolume", sys_get_audio_volume },
    { 20, "SetAudioVolume", sys_set_audio_volume },
    { 37, "GetSettingsItemValueSize", sys_item_value_size },
    { 38, "GetSettingsItemValue", sys_item_value },
    { 41, "GetEdid", sys_get_edid },
    { 42, "SetEdid", sys_set_edid },
    { 43, "GetAudioOutputMode", sys_get_audio_output_mode },
    { 44, "SetAudioOutputMode", sys_set_audio_output_mode },
    { 57, "SetRegionCode", sys_set_region_code },
    { 68, "GetSerialNumber", sys_serial_number },
    { 77, "GetDeviceNickName", get_nickname },
    { 78, "SetDeviceNickName", sys_set_nickname },
    { 82, "AcquireTelemetryDirtyFlagEventHandle", sys_telemetry_event },
    { 83, "GetTelemetryDirtyFlags", sys_dirty_flags },
    { 93, "AcquireFatalDirtyFlagEventHandle", sys_fatal_event },
    { 94, "GetFatalDirtyFlags", sys_dirty_flags },
    { 140, "GetHostFsMountPoint", sys_get_host_fs_mount_point },
};

static const rl_ipc_class sys_class = { "set:sys", sys_cmds, sizeof sys_cmds / sizeof sys_cmds[0], 0x1000, NULL };

static rl_ipc_object *sys_factory(void) { return rl_srv_object(&sys_class, NULL); }

static const rl_ipc_class empty_cal_class = { "set:cal", NULL, 0, 0, NULL };
static const rl_ipc_class empty_fd_class = { "set:fd", NULL, 0, 0, NULL };

static rl_ipc_object *cal_factory(void) { return rl_srv_object(&empty_cal_class, NULL); }

static rl_ipc_object *fd_factory(void) { return rl_srv_object(&empty_fd_class, NULL); }

void rl_srv_install_set(void)
{
    set_reset();
    rl_ipc_register_service("set", set_factory);
    rl_ipc_register_service("set:sys", sys_factory);
    rl_ipc_register_service("set:cal", cal_factory);
    rl_ipc_register_service("set:fd", fd_factory);
}
