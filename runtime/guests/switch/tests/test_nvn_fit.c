/**
 * @file test_nvn_fit.c
 * @brief Whether shader tables fit the run's device (nvn_fit.c): a table made for a device with a feature this one
 * lacks is refused by name before any module is made, the same device or one that offers more is accepted, and a
 * deliberate run can let a mismatch through. The two descriptions are trimmed from the device.json the AYN Thor's
 * Adreno 740 writes on Turnip and on its Qualcomm system driver, whose differences crashed the system driver.
 */
#include "nvn/nvn_module.h"
#include "nvn/nvn_priv.h"
#include "tc.h"

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/** @brief Turnip's description: Int64, the shader clock and subgroup operations in every stage. */
static const char turnip[] =
    "{\n \"capabilities\": {\n  \"device\": {\n   \"extensions\": {\n    \"VK_EXT_shader_atomic_float\": 1,\n"
    "    \"VK_KHR_shader_clock\": 1\n   },\n   \"features\": {\n"
    "    \"VkPhysicalDeviceFeatures\": { \"shaderInt64\": true },\n"
    "    \"VkPhysicalDeviceShaderClockFeaturesKHR\": { \"shaderDeviceClock\": true, \"shaderSubgroupClock\": true },\n"
    "    \"VkPhysicalDeviceVulkan12Features\": { \"shaderBufferInt64Atomics\": true, \"storageBuffer8BitAccess\": false }\n"
    "   },\n   \"properties\": {\n"
    "    \"VkPhysicalDeviceProperties\": { \"deviceName\": \"Turnip Adreno (TM) 740\", \"vendorID\": 20803,\n"
    "     \"limits\": { \"maxComputeWorkGroupInvocations\": 2048, \"maxComputeWorkGroupSize\": [1024, 1024, 1024] } },\n"
    "    \"VkPhysicalDeviceVulkan11Properties\": { \"subgroupSize\": 128,\n"
    "     \"subgroupSupportedStages\": [\"VK_SHADER_STAGE_VERTEX_BIT\", \"VK_SHADER_STAGE_FRAGMENT_BIT\"] },\n"
    "    \"VkPhysicalDeviceVulkan12Properties\": { \"driverID\": \"VK_DRIVER_ID_MESA_TURNIP\" },\n"
    "    \"ReliftDeviceProbes\": { \"canonicalNan32\": false, \"denormFlushMode32\": true }\n"
    "   }\n  }\n },\n \"source\": \"relift runtime, Turnip Adreno (TM) 740\"\n}\n";

/** @brief The system driver's description of the same GPU. */
static const char qualcomm[] =
    "{\n \"capabilities\": {\n  \"device\": {\n   \"extensions\": {\n    \"VK_EXT_shader_atomic_float\": 1\n   },\n"
    "   \"features\": {\n"
    "    \"VkPhysicalDeviceFeatures\": { \"shaderInt64\": false },\n"
    "    \"VkPhysicalDeviceShaderClockFeaturesKHR\": { \"shaderDeviceClock\": false, \"shaderSubgroupClock\": false },\n"
    "    \"VkPhysicalDeviceVulkan12Features\": { \"shaderBufferInt64Atomics\": false, \"storageBuffer8BitAccess\": true }\n"
    "   },\n   \"properties\": {\n"
    "    \"VkPhysicalDeviceProperties\": { \"deviceName\": \"Adreno (TM) 740\", \"vendorID\": 20803,\n"
    "     \"limits\": { \"maxComputeWorkGroupInvocations\": 1024, \"maxComputeWorkGroupSize\": [1024, 1024, 1024] } },\n"
    "    \"VkPhysicalDeviceVulkan11Properties\": { \"subgroupSize\": 64,\n"
    "     \"subgroupSupportedStages\": [\"VK_SHADER_STAGE_FRAGMENT_BIT\"] },\n"
    "    \"VkPhysicalDeviceVulkan12Properties\": { \"driverID\": \"VK_DRIVER_ID_QUALCOMM_PROPRIETARY\" },\n"
    "    \"ReliftDeviceProbes\": { \"canonicalNan32\": false, \"denormFlushMode32\": false }\n"
    "   }\n  }\n },\n \"source\": \"relift runtime, Adreno (TM) 740\"\n}\n";

TEST(nvn_fit_names_every_feature_the_table_device_had_and_this_one_lacks)
{
    nvn_fit f;
    CHECK(nvn_fit_compare(turnip, qualcomm, &f) == 8);
    const char *want[] = { "VK_KHR_shader_clock",
                           "shaderInt64",
                           "shaderDeviceClock",
                           "shaderSubgroupClock",
                           "shaderBufferInt64Atomics",
                           "maxComputeWorkGroupInvocations 1024 here",
                           "subgroupSize 64 here",
                           "subgroupSupportedStages VK_SHADER_STAGE_VERTEX_BIT" };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK(strstr(f.missing, want[i]) != NULL);
    CHECK(strstr(f.missing, "storageBuffer8BitAccess") == NULL);
    CHECK(strstr(f.missing, "denormFlushMode32") == NULL);
    CHECK(strstr(f.other, "denormFlushMode32 false here") != NULL);
    CHECK(strstr(f.other, "deviceName Adreno (TM) 740") != NULL);
    CHECK(!strcmp(f.table_device, "Turnip Adreno (TM) 740 (VK_DRIVER_ID_MESA_TURNIP)"));
    CHECK(!strcmp(f.this_device, "Adreno (TM) 740 (VK_DRIVER_ID_QUALCOMM_PROPRIETARY)"));
}

TEST(nvn_fit_takes_the_same_device_and_one_that_offers_more)
{
    nvn_fit f;
    CHECK(nvn_fit_compare(turnip, turnip, &f) == 0);
    CHECK(f.other_count == 0);
    CHECK(nvn_fit_compare(qualcomm, qualcomm, &f) == 0);
    /* The system driver's table on Turnip: Turnip lacks storageBuffer8BitAccess, and its subgroups are another size. */
    CHECK(nvn_fit_compare(qualcomm, turnip, &f) == 2);
    CHECK(!strcmp(f.missing, "storageBuffer8BitAccess, subgroupSize 128 here"));
    /* A device that offers more than the table's (here only a larger limit) fits. */
    char more[sizeof qualcomm + 8];
    snprintf(more, sizeof more, "%s", qualcomm);
    char *lim = strstr(more, "\"maxComputeWorkGroupInvocations\": 1024");
    CHECK(lim != NULL);
    memcpy(lim + strlen("\"maxComputeWorkGroupInvocations\": "), "4096", 4);
    CHECK(nvn_fit_compare(qualcomm, more, &f) == 0);
    CHECK(strstr(f.other, "maxComputeWorkGroupInvocations 4096 here") != NULL);
}

/** @brief With subgroupSizeControl the translation picks its size within min and max, so subgroupSize alone may differ. */
TEST(nvn_fit_takes_another_subgroup_size_when_the_size_is_controlled)
{
    static const char a[] = "{\"p\": { \"VkPhysicalDeviceVulkan13Features\": { \"subgroupSizeControl\": true },"
                            " \"VkPhysicalDeviceVulkan11Properties\": { \"subgroupSize\": 128 },"
                            " \"VkPhysicalDeviceVulkan13Properties\": { \"minSubgroupSize\": 64, \"maxSubgroupSize\": 128 } } }";
    static const char b[] = "{\"p\": { \"VkPhysicalDeviceVulkan13Features\": { \"subgroupSizeControl\": true },"
                            " \"VkPhysicalDeviceVulkan11Properties\": { \"subgroupSize\": 64 },"
                            " \"VkPhysicalDeviceVulkan13Properties\": { \"minSubgroupSize\": 64, \"maxSubgroupSize\": 128 } } }";
    static const char c[] = "{\"p\": { \"VkPhysicalDeviceVulkan13Features\": { \"subgroupSizeControl\": true },"
                            " \"VkPhysicalDeviceVulkan11Properties\": { \"subgroupSize\": 64 },"
                            " \"VkPhysicalDeviceVulkan13Properties\": { \"minSubgroupSize\": 64, \"maxSubgroupSize\": 64 } } }";
    nvn_fit f;
    CHECK(nvn_fit_compare(a, b, &f) == 0);
    CHECK(strstr(f.other, "subgroupSize 64 here") != NULL);
    CHECK(nvn_fit_compare(a, c, &f) == 1);
    CHECK(!strcmp(f.missing, "maxSubgroupSize 64 here"));
}

TEST(nvn_fit_refuses_text_that_is_not_device_json)
{
    nvn_fit f;
    CHECK(nvn_fit_compare("{ \"capabilities\": ", qualcomm, &f) == 1);
    CHECK(strstr(f.missing, "table.device is not device JSON") != NULL);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void put(const char *dir, const char *name, const char *text)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

TEST(nvn_tables_made_for_another_driver_stop_before_any_module_by_name)
{
    char dir[PATH_MAX], why[3072] = "", key[65];
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/relift-fit-XXXXXX", base && *base ? base : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    setenv("RELIFT_NVN_TABLES", dir, 1);
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    unsetenv("RELIFT_NVN_TABLE_STAMP");
    unsetenv("RELIFT_NVN_TABLE_DEVICE");
    put(dir, "table.stamp", "0123456789abcdef\n");
    put(dir, "table.device", turnip);
    put(dir, "device.json", qualcomm);
    rl_nvn_set_table_stamp("0123456789abcdef");
    nvn_shader_tables_repick();
    CHECK(!nvn_shader_stamp_check(why, sizeof why));
    CHECK(strstr(why, "made for Turnip Adreno (TM) 740 (VK_DRIVER_ID_MESA_TURNIP)") != NULL);
    CHECK(strstr(why, "Adreno (TM) 740 (VK_DRIVER_ID_QUALCOMM_PROPRIETARY), lacks: VK_KHR_shader_clock, shaderInt64") !=
          NULL);
    memset(key, '0', 64);
    key[64] = 0;
    why[0] = 0;
    CHECK(nvn_module_get(key, (int)NVN_STAGE_VERTEX, why, sizeof why) == NULL);
    CHECK(strstr(why, "shaderInt64") != NULL);
    /* A deliberate run lets it through; tables made for this device are taken as they are. */
    setenv("RELIFT_NVN_TABLE_DEVICE", "allow", 1);
    nvn_shader_tables_repick();
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    unsetenv("RELIFT_NVN_TABLE_DEVICE");
    put(dir, "table.device", qualcomm);
    nvn_shader_tables_repick();
    CHECK(nvn_shader_stamp_check(why, sizeof why));
    rl_nvn_set_table_stamp(NULL);
    nvn_shader_tables_repick();
    unsetenv("RELIFT_NVN_TABLES");
    unsetenv("RELIFT_NVN_SHADERS");
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_compile_check_children_are_sized_from_the_memory_available)
{
    const uint64_t gb = 1ull << 30;
    /* Nothing measured yet: one child, and none when the reserve is not there. */
    CHECK(nvn_check_children_fit(13 * gb, 4 * gb, 0, 0, 4) == 1);
    CHECK(nvn_check_children_fit(13 * gb, 4 * gb, 0, 1, 4) == 0);
    CHECK(nvn_check_children_fit(3 * gb, 4 * gb, 0, 0, 4) == 0);
    /* The Thor: 13 GB available, children of 2 GB, 4 GB kept free: four fit, never more than wanted. */
    CHECK(nvn_check_children_fit(13 * gb, 4 * gb, 2 * gb, 0, 8) == 4);
    CHECK(nvn_check_children_fit(13 * gb, 4 * gb, 2 * gb, 0, 2) == 2);
    CHECK(nvn_check_children_fit(13 * gb, 4 * gb, 2 * gb, 2, 2) == 0);
    /* What the running ones still take is already out of avail: 5 GB left fits none more of 2 GB with 4 kept. */
    CHECK(nvn_check_children_fit(5 * gb, 4 * gb, 2 * gb, 1, 4) == 0);
}
