/**
 * @file test_settings.c
 * @brief The settings registry: types and ranges, layers and precedence,
 * the JSON file, the command line, unknown keys, the schema and the
 * per-user folder per host system.
 */
#include "runner.h"

#include <relift/settings.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const g_modes[] = { "off", "low", "high" };

static const rl_setting_def g_defs[] = {
    { .key = "hud", .type = RL_SETTING_BOOL, .label = "HUD", .def_int = 1 },
    { .key = "scale", .type = RL_SETTING_INT, .min_int = 1, .max_int = 9, .step_int = 2, .def_int = 3 },
    { .key = "gain", .type = RL_SETTING_FLOAT, .min_float = 0, .max_float = 1, .step_float = 0.25, .def_float = 0.5 },
    { .key = "mode", .type = RL_SETTING_ENUM, .choices = g_modes, .nchoices = 3, .def_int = 1 },
    { .key = "name", .type = RL_SETTING_STRING, .def_str = "anon", .env = "RL_TEST_NAME" },
    { .key = "jump", .type = RL_SETTING_BINDING, .def_str = "pad:a" },
    { .key = "mod.fast", .type = RL_SETTING_BOOL, .apply = RL_APPLY_START },
};
#define NDEFS (sizeof g_defs / sizeof g_defs[0])

static void fresh(void)
{
    rl_settings_reset();
    unsetenv("RL_TEST_NAME");
    CHECK_EQ(rl_settings_declare(g_defs, NDEFS), (int)NDEFS);
}

static char *write_tmp(const char *text)
{
    static char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/rl_settings_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    if (write(fd, text, strlen(text)) < 0) {
        close(fd);
        return NULL;
    }
    close(fd);
    return path;
}

TEST(settings_defaults)
{
    fresh();
    CHECK(rl_setting_bool("hud"));
    CHECK_EQ(rl_setting_int("scale"), 3);
    CHECK(rl_setting_float("gain") == 0.5);
    CHECK_EQ(rl_setting_enum("mode"), 1);
    CHECK(!strcmp(rl_setting_str("name"), "anon"));
    const char *port, *control;
    CHECK(rl_setting_binding("jump", &port, &control));
    CHECK(!strcmp(port, "pad") && !strcmp(control, "a"));
    CHECK(!rl_setting_bool("mod.fast"));
}

TEST(settings_types_ranges_and_steps)
{
    fresh();
    CHECK_EQ(rl_settings_set("hud", "off"), RL_SETTING_OK);
    CHECK(!rl_setting_bool("hud"));
    CHECK_EQ(rl_settings_set("hud", "maybe"), RL_SETTING_INVALID);
    CHECK(!rl_setting_bool("hud"));
    CHECK_EQ(rl_settings_set("scale", "9"), RL_SETTING_OK);
    CHECK_EQ(rl_setting_int("scale"), 9);
    CHECK_EQ(rl_settings_set("scale", "10"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("scale", "0"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("scale", "4"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("scale", "3x"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("scale", ""), RL_SETTING_INVALID);
    CHECK_EQ(rl_setting_int("scale"), 9);
    CHECK_EQ(rl_settings_set("gain", "0.75"), RL_SETTING_OK);
    CHECK(rl_setting_float("gain") == 0.75);
    CHECK_EQ(rl_settings_set("gain", "0.8"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("gain", "1.25"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("gain", "nan"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("mode", "high"), RL_SETTING_OK);
    CHECK_EQ(rl_setting_enum("mode"), 2);
    CHECK_EQ(rl_settings_set("mode", "extreme"), RL_SETTING_INVALID);
    CHECK_EQ(rl_setting_enum("mode"), 2);
    CHECK_EQ(rl_settings_set("jump", "pad"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("jump", ":a"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("jump", "pad:"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("jump", "pad:a:b"), RL_SETTING_INVALID);
    CHECK_EQ(rl_settings_set("jump", "pad2:start"), RL_SETTING_OK);
    const char *port, *control;
    CHECK(rl_setting_binding("jump", &port, &control));
    CHECK(!strcmp(port, "pad2") && !strcmp(control, "start"));
}

TEST(settings_unknown_keys_warn_and_never_fail)
{
    fresh();
    CHECK_EQ(rl_settings_set("nope", "1"), RL_SETTING_UNKNOWN);
    CHECK(!rl_setting_bool("nope"));
    CHECK(!rl_setting_bool("scale"));
    CHECK_EQ(rl_setting_int("hud"), 0);
    char *argv[] = { "x", "--relift-set", "nope=1", "--relift-set", "scale=5", "--relift-set", "bad", NULL };
    CHECK_EQ(rl_settings_args(7, argv), 1);
    CHECK_EQ(rl_setting_int("scale"), 5);
}

TEST(settings_json_file)
{
    fresh();
    const char *text = "{ \"hud\": false, \"scale\": 7, \"gain\": 0.25, \"mode\": \"low\","
                       " \"name\": \"a\\\"b\\u00e9\", \"jump\": \"p:x\", \"mod.fast\": true, \"extra\": 1, \"scale2\": null }";
    CHECK_EQ(rl_settings_load_json(text, "t"), 7);
    CHECK(!rl_setting_bool("hud"));
    CHECK_EQ(rl_setting_int("scale"), 7);
    CHECK(rl_setting_float("gain") == 0.25);
    CHECK_EQ(rl_setting_enum("mode"), 1);
    CHECK(!strcmp(rl_setting_str("name"), "a\"b\xc3\xa9"));
    CHECK(rl_setting_bool("mod.fast"));
    CHECK_EQ(rl_settings_load_json("{}", "t"), 0);
    CHECK_EQ(rl_settings_load_json("[1]", "t"), -1);
    CHECK_EQ(rl_settings_load_json("{\"hud\": }", "t"), -1);
    CHECK_EQ(rl_settings_load_json("{\"hud\": true} x", "t"), -1);
    CHECK_EQ(rl_settings_load_json("{\"name\": \"open", "t"), -1);
    CHECK_EQ(rl_settings_load_file("/nonexistent/rl.json"), -1);
}

TEST(settings_set_flag_beats_file_whatever_the_order)
{
    fresh();
    char *f = write_tmp("{\"scale\": 3, \"gain\": 1, \"mode\": \"off\"}");
    CHECK(f != NULL);
    char *argv[] = { "x", "--relift-set", "scale=5", "--relift-settings", f, "--relift-set=gain=0.25",
                     "--relift-set", "scale=7", NULL };
    CHECK_EQ(rl_settings_args(8, argv), 6);
    unlink(f);
    CHECK_EQ(rl_setting_int("scale"), 7);
    CHECK(rl_setting_float("gain") == 0.25);
    CHECK_EQ(rl_setting_enum("mode"), 0);
}

TEST(settings_later_file_wins_and_a_missing_file_is_skipped)
{
    fresh();
    char *a = strdup(write_tmp("{\"scale\": 3}"));
    char *b = strdup(write_tmp("{\"scale\": 5}"));
    char *argv[] = { "x", "--relift-settings", "/nonexistent/x.json", "--relift-settings", a, "--relift-settings=",
                     "--relift-settings", b, NULL };
    CHECK(rl_settings_args(8, argv) >= 2);
    unlink(a);
    unlink(b);
    free(a);
    free(b);
    CHECK_EQ(rl_setting_int("scale"), 5);
}

TEST(settings_environment_overrides_every_layer)
{
    fresh();
    CHECK_EQ(rl_settings_set("name", "flag"), RL_SETTING_OK);
    CHECK(!strcmp(rl_setting_str("name"), "flag"));
    setenv("RL_TEST_NAME", "env", 1);
    CHECK(!strcmp(rl_setting_str("name"), "env"));
    setenv("RL_TEST_NAME", "env2", 1);
    CHECK(!strcmp(rl_setting_str("name"), "env2"));
    unsetenv("RL_TEST_NAME");
    CHECK(!strcmp(rl_setting_str("name"), "flag"));
}

TEST(settings_invalid_environment_is_ignored)
{
    fresh();
    static const rl_setting_def d = { .key = "lvl", .type = RL_SETTING_INT, .min_int = 0, .max_int = 3, .env = "RL_TEST_LVL" };
    CHECK_EQ(rl_settings_declare(&d, 1), 1);
    CHECK_EQ(rl_settings_set("lvl", "2"), RL_SETTING_OK);
    setenv("RL_TEST_LVL", "99", 1);
    CHECK_EQ(rl_setting_int("lvl"), 2);
    setenv("RL_TEST_LVL", "1", 1);
    CHECK_EQ(rl_setting_int("lvl"), 1);
    unsetenv("RL_TEST_LVL");
}

TEST(settings_declare_checks_defs)
{
    rl_settings_reset();
    static const rl_setting_def dup[] = { { .key = "a", .type = RL_SETTING_BOOL }, { .key = "a", .type = RL_SETTING_BOOL } };
    CHECK_EQ(rl_settings_declare(dup, 2), -1);
    static const rl_setting_def badkey = { .key = "Bad", .type = RL_SETTING_BOOL };
    CHECK_EQ(rl_settings_declare(&badkey, 1), -1);
    static const rl_setting_def badrange = { .key = "r", .type = RL_SETTING_INT, .min_int = 5, .max_int = 1, .def_int = 5 };
    CHECK_EQ(rl_settings_declare(&badrange, 1), -1);
    static const rl_setting_def outside = { .key = "r", .type = RL_SETTING_INT, .min_int = 1, .max_int = 5, .def_int = 6 };
    CHECK_EQ(rl_settings_declare(&outside, 1), -1);
    static const rl_setting_def noch = { .key = "e", .type = RL_SETTING_ENUM };
    CHECK_EQ(rl_settings_declare(&noch, 1), -1);
    CHECK_EQ(rl_settings_declare(g_defs, NDEFS), (int)NDEFS);
    CHECK_EQ(rl_settings_declare(g_defs, NDEFS), 0);
    static const rl_setting_def again = { .key = "hud", .type = RL_SETTING_BOOL };
    CHECK_EQ(rl_settings_declare(&again, 1), -1);
}

TEST(settings_schema_json)
{
    fresh();
    char buf[4096];
    size_t n = rl_settings_schema_json(buf, sizeof buf);
    CHECK_EQ(n, strlen(buf));
    CHECK(strstr(buf, "{\"key\":\"hud\",\"type\":\"bool\",\"label\":\"HUD\",\"description\":\"\",\"apply\":\"start\",\"default\":true}"));
    CHECK(strstr(buf, "\"key\":\"scale\",\"type\":\"int\""));
    CHECK(strstr(buf, "\"default\":3,\"min\":1,\"max\":9,\"step\":2"));
    CHECK(strstr(buf, "\"default\":\"low\",\"choices\":[\"off\",\"low\",\"high\"]"));
    CHECK(strstr(buf, "\"env\":\"RL_TEST_NAME\""));
    CHECK(strstr(buf, "\"type\":\"key_binding\""));
    CHECK(strstr(buf, "\"default\":{\"port\":\"pad\",\"control\":\"a\"}"));
    CHECK(buf[0] == '[' && buf[n - 1] == ']');
    char small[16];
    CHECK_EQ(rl_settings_schema_json(small, sizeof small), n);
    CHECK_EQ(strlen(small), sizeof small - 1);
    CHECK_EQ(rl_settings_schema_json(NULL, 0), n);
}

TEST(settings_user_dir_per_host_system)
{
    char out[256];
    CHECK(rl_settings_dir_for(out, sizeof out, RL_DIR_XDG, "game", "/home/u", NULL, NULL));
    CHECK(!strcmp(out, "/home/u/.local/share/game"));
    CHECK(rl_settings_dir_for(out, sizeof out, RL_DIR_XDG, "game", "/home/u", "/data/", NULL));
    CHECK(!strcmp(out, "/data/game"));
    CHECK(rl_settings_dir_for(out, sizeof out, RL_DIR_XDG, "game", "/home/u", "relative", NULL));
    CHECK(!strcmp(out, "/home/u/.local/share/game"));
    CHECK(!rl_settings_dir_for(out, sizeof out, RL_DIR_XDG, "game", NULL, NULL, NULL));
    CHECK(rl_settings_dir_for(out, sizeof out, RL_DIR_MACOS, "game", "/Users/u", "/ignored", NULL));
    CHECK(!strcmp(out, "/Users/u/Library/Application Support/game"));
    CHECK(!rl_settings_dir_for(out, sizeof out, RL_DIR_MACOS, "game", "", NULL, NULL));
    CHECK(rl_settings_dir_for(out, sizeof out, RL_DIR_WINDOWS, "game", "C:\\Users\\u", NULL, "C:\\Users\\u\\AppData\\Roaming"));
    CHECK(!strcmp(out, "C:\\Users\\u\\AppData\\Roaming\\game"));
    CHECK(rl_settings_dir_for(out, sizeof out, RL_DIR_WINDOWS, "game", "C:\\Users\\u", NULL, NULL));
    CHECK(!strcmp(out, "C:\\Users\\u\\AppData\\Roaming\\game"));
    CHECK(!rl_settings_dir_for(out, sizeof out, RL_DIR_WINDOWS, "game", NULL, NULL, NULL));
    CHECK(!rl_settings_dir_for(out, 8, RL_DIR_XDG, "game", "/home/u", NULL, NULL));
    CHECK(!rl_settings_dir_for(out, sizeof out, RL_DIR_XDG, "../x", "/home/u", NULL, NULL));
    CHECK(!rl_settings_dir_for(out, sizeof out, RL_DIR_XDG, "", "/home/u", NULL, NULL));
}

TEST(settings_user_file_is_read_only_with_an_app_and_no_launcher_file)
{
    fresh();
    char dir[256], path[300], cmd[600];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rl_home_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p %s/.local/share/g && printf '{\"scale\": 5}' > %s/.local/share/g/settings.json", dir, dir);
    CHECK_EQ(system(cmd), 0);
    setenv("HOME", dir, 1);
    unsetenv("XDG_DATA_HOME");
    char *argv[] = { "x", NULL };
    CHECK_EQ(rl_settings_args(1, argv), 0);
    rl_settings_set_app("g");
    CHECK(rl_settings_user_file(path, sizeof path));
    CHECK_EQ(rl_settings_args(1, argv), 1);
    CHECK_EQ(rl_setting_int("scale"), 5);
    CHECK_EQ(rl_settings_set("scale", "1"), RL_SETTING_OK);
    char *f = write_tmp("{\"gain\": 1}");
    char *argv2[] = { "x", "--relift-settings", f, NULL };
    CHECK_EQ(rl_settings_args(3, argv2), 1);
    unlink(f);
    CHECK_EQ(rl_setting_int("scale"), 1);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK_EQ(system(cmd), 0);
}
