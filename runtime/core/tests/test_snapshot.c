/**
 * @file test_snapshot.c
 * @brief The diagnostic snapshot: untriggered presents write nothing; a request (and the snapshot-now file) starts a
 * window of debug.snapshot presents; the folder then holds every file the index lists, the core's and a headless test
 * guest's own provider's, each filled; a second request after the window makes a second folder; debug.snapshot=0
 * turns the triggers off.
 */
#include "runner.h"

#include "gdbstub.h"
#include "snapshot.h"

#include <relift/settings.h>

#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(RL_DEBUG_STUB)

static char g_root[512];
static int g_begins, g_presents, g_ends;

/** @brief A provider of a headless test guest: arms on begin, counts presents, writes one file on end. */
static void guest_begin(void *user, const char *dir, uint32_t presents)
{
    (void)user, (void)dir, (void)presents;
    g_begins++;
}

static void guest_present(void *user, const char *dir, uint32_t index)
{
    (void)user, (void)dir, (void)index;
    g_presents++;
}

static void guest_end(void *user, const char *dir)
{
    (void)user;
    char path[700];
    snprintf(path, sizeof path, "%s/guest_threads.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "thread 1 waiting in a test event\n");
    fclose(f);
    rl_snapshot_file("guest_threads.txt", "The test guest's threads.");
    g_ends++;
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)type;
    (void)ftw;
    return remove(path);
}

/** @brief Deletes the scratch folder at path, if any. */
static void remove_tree(const char *path)
{
    nftw(path, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

/**
 * @brief Makes a fresh scratch folder named relift-snapshot-scratch under the build folder (git-ignored, never the
 * RAM-backed temp dir). Each test runs in its own process and leaves by _exit, so the folder is removed here, at the
 * start of the next setup, instead of at exit; at most one stays behind.
 */
static void setup(void)
{
    mkdir("build", 0777);
    snprintf(g_root, sizeof g_root, "build/relift-snapshot-scratch");
    remove_tree(g_root);
    if (mkdir(g_root, 0777) != 0)
        g_root[0] = 0;
    rl_settings_declare_runtime();
    rl_snapshot_set_root(g_root);
    rl_snapshot_provider p = { .name = "test", .begin = guest_begin, .present = guest_present, .end = guest_end };
    rl_snapshot_register(&p);
}

/** @brief The number of entries in <root>/snapshots, or 0 when it does not exist. */
static int snapshot_folders(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/snapshots", g_root);
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.')
            n++;
    if (d)
        closedir(d);
    return n;
}

static long file_size(const char *dir, const char *name)
{
    char path[1400];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

TEST(snapshot_writes_nothing_until_triggered)
{
    setup();
    CHECK(g_root[0]);
    for (int i = 0; i < 20; i++)
        rl_snapshot_present();
    CHECK(!rl_snapshot_active());
    CHECK_EQ(snapshot_folders(), 0);
    CHECK_EQ(g_begins, 0);
}

TEST(snapshot_request_writes_every_listed_file_after_the_window)
{
    setup();
    CHECK(g_root[0]);
    rl_log(RL_LOG_INFO, "test", "a line the log tail must hold");
    CHECK_EQ(rl_settings_set("debug.snapshot", "3"), RL_SETTING_OK);
    rl_snapshot_present();
    rl_snapshot_request();
    rl_snapshot_present(); /* the trigger present: the window opens */
    CHECK(rl_snapshot_active());
    CHECK_EQ(g_begins, 1);
    rl_snapshot_present();
    rl_snapshot_present();
    CHECK(rl_snapshot_active());
    rl_snapshot_present(); /* the third present of the window */
    CHECK(!rl_snapshot_active());
    CHECK_EQ(g_presents, 3);
    CHECK_EQ(g_ends, 1);
    CHECK_EQ(snapshot_folders(), 1);

    const char *dir = rl_snapshot_last_dir();
    CHECK(dir[0]);
    char path[1400];
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "r");
    CHECK(f);
    char line[600];
    int listed = 0;
    bool saw_guest = false;
    while (fgets(line, sizeof line, f)) {
        /* A file line is indented two spaces and names a file; its description is indented further. */
        if (line[0] == ' ' && line[1] == ' ' && line[2] != ' ') {
            line[strcspn(line, "\n")] = 0;
            listed++;
            CHECK(file_size(dir, line + 2) > 0);
            saw_guest |= !strcmp(line + 2, "guest_threads.txt");
        }
    }
    fclose(f);
    CHECK(saw_guest);
    /* index, settings, build, memory, heap, host threads, log tail and the guest's file; no picture without a window. */
    CHECK(listed >= 8);
    CHECK(file_size(dir, "settings.txt") > 0);
    CHECK(file_size(dir, "log_tail.txt") > 0);
    CHECK(file_size(dir, "threads_host.txt") > 0);
    CHECK(file_size(dir, "memory.txt") > 0);

    snprintf(path, sizeof path, "%s/log_tail.txt", dir);
    f = fopen(path, "r");
    CHECK(f);
    bool tail = false;
    while (fgets(line, sizeof line, f))
        tail |= strstr(line, "a line the log tail must hold") != NULL;
    fclose(f);
    CHECK(tail);
}

TEST(snapshot_file_trigger_starts_a_window_and_is_taken)
{
    setup();
    CHECK(g_root[0]);
    CHECK_EQ(rl_settings_set("debug.snapshot", "1"), RL_SETTING_OK);
    char path[600];
    snprintf(path, sizeof path, "%s/snapshot-now", g_root);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fclose(f);
    /* The file is looked for at most twice a second; a few presents over a second find it. */
    for (int i = 0; i < 40 && !rl_snapshot_active() && !snapshot_folders(); i++) {
        rl_snapshot_present();
        usleep(40000);
    }
    CHECK(rl_snapshot_active() || snapshot_folders() == 1);
    CHECK(access(path, F_OK) != 0);
    rl_snapshot_present();
    CHECK(!rl_snapshot_active());
    CHECK_EQ(snapshot_folders(), 1);
}

TEST(snapshot_setting_zero_turns_the_triggers_off)
{
    setup();
    CHECK(g_root[0]);
    CHECK_EQ(rl_settings_set("debug.snapshot", "0"), RL_SETTING_OK);
    rl_snapshot_request();
    for (int i = 0; i < 5; i++)
        rl_snapshot_present();
    CHECK(!rl_snapshot_active());
    CHECK_EQ(snapshot_folders(), 0);
}

#endif
