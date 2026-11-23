/**
 * @file test_warmup.c
 * @brief The compile screen: the rate and time left from measured counts, the words, the state machine from
 * measuring to done, the blocking wait and its answers, the corner card's life, and the pixels the screen draws.
 */
#include "runner.h"

#include <relift/warmup.h>

#include "warmup_int.h"

#include <stdlib.h>
#include <string.h>

#define SEC 1000000000ull

static uint64_t g_now;
static int g_quits, g_bases, g_wakes;
static bool g_base_on;

static uint64_t fake_clock(void) { return g_now; }
static void host_quit(void) { g_quits++; }
static void host_wake(void) { g_wakes++; }
static void host_base(bool on)
{
    g_bases++;
    g_base_on = on;
}

/** @brief A fresh screen on a fake clock at 100 s, with a host that counts what it is asked. */
static void fresh(void)
{
    rl_warmup_reset();
    rl_wu_test_clock(fake_clock);
    g_now = 100 * SEC;
    g_quits = g_bases = g_wakes = 0;
    g_base_on = false;
    rl_warmup_host h = { .base = host_base, .quit = host_quit, .wake = host_wake };
    rl_warmup_set_host(&h);
    rl_warmup_view(1920, 1080);
}

static const uint8_t *draw(int32_t dirty[4])
{
    uint32_t w, h;
    return rl_warmup_pixels(g_now, &w, &h, dirty);
}

static uint32_t alpha_at(const uint8_t *px, uint32_t x, uint32_t y) { return px[((size_t)y * 1920 + x) * 4 + 3]; }

TEST(the_time_left_is_the_average_rate_so_far_rounded_up)
{
    uint32_t s = 0;
    CHECK(rl_wu_eta(100, 1000, 10 * SEC, &s));
    CHECK_EQ(s, 90);
    CHECK(rl_wu_eta(333, 1000, 10 * SEC, &s));
    CHECK_EQ(s, 21);
    CHECK(rl_wu_eta(1240, 2077, 134 * SEC, &s));
    CHECK_EQ(s, 91);
}

TEST(no_time_is_given_before_five_seconds_or_twenty_shaders)
{
    uint32_t s = 7;
    CHECK(!rl_wu_eta(100, 1000, 4 * SEC + 999999999, &s));
    CHECK(rl_wu_eta(100, 1000, 5 * SEC, &s));
    CHECK(!rl_wu_eta(19, 1000, 60 * SEC, &s));
    CHECK(rl_wu_eta(20, 1000, 60 * SEC, &s));
    CHECK(!rl_wu_eta(0, 1000, 60 * SEC, &s));
    CHECK_EQ(s, 2940);
}

TEST(a_small_queue_trusts_half_of_it_and_a_finished_one_has_no_time_left)
{
    uint32_t s = 0;
    CHECK(!rl_wu_eta(4, 10, 30 * SEC, &s));
    CHECK(rl_wu_eta(5, 10, 30 * SEC, &s));
    CHECK_EQ(s, 30);
    CHECK(!rl_wu_eta(10, 10, 30 * SEC, &s));
    CHECK(!rl_wu_eta(0, 1, 30 * SEC, &s));
    CHECK(rl_wu_eta(1, 2, 30 * SEC, &s));
    CHECK(!rl_wu_eta(1, 0, 30 * SEC, &s));
}

TEST(the_time_left_reads_in_seconds_under_a_minute_and_whole_minutes_above)
{
    char b[64];
    rl_wu_eta_text(1, b, sizeof b);
    CHECK(strcmp(b, "About 1 second left") == 0);
    rl_wu_eta_text(40, b, sizeof b);
    CHECK(strcmp(b, "About 40 seconds left") == 0);
    rl_wu_eta_text(59, b, sizeof b);
    CHECK(strcmp(b, "About 59 seconds left") == 0);
    rl_wu_eta_text(60, b, sizeof b);
    CHECK(strcmp(b, "About 1 minute left") == 0);
    rl_wu_eta_text(89, b, sizeof b);
    CHECK(strcmp(b, "About 1 minute left") == 0);
    rl_wu_eta_text(91, b, sizeof b);
    CHECK(strcmp(b, "About 2 minutes left") == 0);
    rl_wu_eta_text(7000, b, sizeof b);
    CHECK(strcmp(b, "About 117 minutes left") == 0);
    rl_wu_eta_text(7200, b, sizeof b);
    CHECK(strcmp(b, "About 2 hours left") == 0);
}

TEST(counts_times_and_percents_are_written_plainly)
{
    char b[64];
    rl_wu_count_text(0, b, sizeof b);
    CHECK(strcmp(b, "0") == 0);
    rl_wu_count_text(999, b, sizeof b);
    CHECK(strcmp(b, "999") == 0);
    rl_wu_count_text(2077, b, sizeof b);
    CHECK(strcmp(b, "2,077") == 0);
    rl_wu_count_text(12255, b, sizeof b);
    CHECK(strcmp(b, "12,255") == 0);
    rl_wu_count_text(1234567, b, sizeof b);
    CHECK(strcmp(b, "1,234,567") == 0);
    rl_wu_time_text(0, b, sizeof b);
    CHECK(strcmp(b, "0:00") == 0);
    rl_wu_time_text(134, b, sizeof b);
    CHECK(strcmp(b, "2:14") == 0);
    rl_wu_time_text(3725, b, sizeof b);
    CHECK(strcmp(b, "1:02:05") == 0);
    CHECK_EQ(rl_wu_percent(1240, 2077), 59);
    CHECK_EQ(rl_wu_percent(2077, 2077), 100);
    CHECK_EQ(rl_wu_percent(1, 0), 0);
    CHECK_EQ(rl_wu_percent(1, 1000), 0);
}

TEST(nothing_shows_until_a_compile_begins)
{
    fresh();
    int32_t d[4];
    CHECK(!rl_warmup_active());
    CHECK(draw(d) == NULL);
    rl_warmup_begin(0, RL_WARMUP_BLOCKING);
    CHECK(!rl_warmup_active());
    CHECK(draw(d) == NULL);
    rl_warmup_reset();
}

TEST(the_blocking_screen_covers_the_window_and_asks_the_host_for_a_base)
{
    fresh();
    rl_warmup_begin(2077, RL_WARMUP_BLOCKING);
    CHECK(rl_warmup_blocking());
    CHECK(g_base_on);
    int32_t d[4];
    const uint8_t *px = draw(d);
    CHECK(px != NULL);
    CHECK_EQ(alpha_at(px, 0, 0), 255);
    CHECK_EQ(alpha_at(px, 1919, 1079), 255);
    CHECK_EQ(d[0], 0);
    CHECK_EQ(d[2], 1920);
    CHECK_EQ(d[3], 1080);
    rl_warmup_game_started();
    CHECK(!g_base_on);
    rl_warmup_reset();
}

TEST(the_corner_card_draws_only_itself_and_leaves_the_game_visible)
{
    fresh();
    rl_warmup_begin(2077, RL_WARMUP_BACKGROUND);
    CHECK(g_base_on);
    CHECK(!rl_warmup_blocking());
    int32_t d[4];
    const uint8_t *px = draw(d);
    CHECK(px != NULL);
    CHECK_EQ(alpha_at(px, 10, 1070), 255);
    /* Held first submit: the card takes no key or tap, the game has not started to take them either. */
    CHECK(!rl_warmup_key(RL_WARMUP_KEY_ESCAPE));
    CHECK_EQ(g_quits, 0);
    rl_warmup_game_started();
    CHECK(!g_base_on);
    px = draw(d);
    CHECK(px != NULL);
    CHECK_EQ(alpha_at(px, 10, 1070), 0);
    CHECK_EQ(alpha_at(px, 960, 540), 0);
    int32_t e[4];
    rl_warmup_extent(e);
    CHECK(e[2] - e[0] > 0 && e[2] - e[0] < 1920 / 2);
    CHECK(e[3] - e[1] > 0 && e[3] - e[1] < 1080 / 2);
    CHECK(e[0] > 1920 / 2);
    rl_warmup_reset();
}

TEST(the_corner_card_is_centered_on_a_tall_window)
{
    fresh();
    rl_warmup_view(1080, 2400);
    rl_warmup_begin(2077, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    int32_t d[4];
    uint32_t w, h;
    const uint8_t *px = rl_warmup_pixels(g_now, &w, &h, d);
    CHECK(px != NULL);
    CHECK_EQ(w, 1080);
    int32_t e[4];
    rl_warmup_extent(e);
    CHECK(e[0] > 0 && e[2] < 1080);
    CHECK((e[0] + e[2]) / 2 > 1080 / 2 - 8 && (e[0] + e[2]) / 2 < 1080 / 2 + 8);
    rl_warmup_reset();
}

TEST(a_screen_is_measuring_until_the_rate_is_measured_then_compiling)
{
    fresh();
    rl_warmup_begin(1000, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    rl_warmup_progress(100, 0);
    int32_t d[4];
    g_now += 4 * SEC;
    CHECK(draw(d) != NULL);
    uint64_t first = (uint64_t)d[2] * (uint64_t)d[3];
    CHECK(first > 0);
    g_now += 2 * SEC;
    rl_warmup_progress(150, 0);
    CHECK(draw(d) != NULL);
    CHECK(d[2] > 0);
    rl_warmup_reset();
}

TEST(progress_after_the_end_does_not_move_a_finished_screen)
{
    fresh();
    rl_warmup_begin(10, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    int32_t d[4];
    CHECK(draw(d) != NULL);
    rl_warmup_progress(10, 0);
    rl_warmup_saving();
    rl_warmup_end();
    rl_warmup_progress(3, 3);
    CHECK(draw(d) != NULL);
    g_now += RL_WU_READY_NS - 1;
    CHECK(rl_warmup_active());
    rl_warmup_reset();
}

TEST(the_ready_card_goes_away_after_five_seconds)
{
    fresh();
    rl_warmup_begin(50, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    int32_t d[4];
    CHECK(draw(d) != NULL);
    rl_warmup_progress(50, 0);
    rl_warmup_saving();
    rl_warmup_end();
    CHECK(draw(d) != NULL);
    g_now += 4 * SEC;
    CHECK(draw(d) != NULL);
    g_now += 2 * SEC;
    CHECK(draw(d) == NULL);
    CHECK(!rl_warmup_active());
    rl_warmup_reset();
}

TEST(a_compile_that_ends_before_the_corner_card_shows_leaves_nothing_to_say)
{
    fresh();
    rl_warmup_begin(50, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    rl_warmup_progress(50, 0);
    rl_warmup_saving();
    rl_warmup_end();
    int32_t d[4];
    CHECK(!rl_warmup_active());
    CHECK(draw(d) == NULL);
    fresh();
    rl_warmup_begin(50, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    rl_warmup_progress(50, 2);
    rl_warmup_saving();
    rl_warmup_end();
    CHECK(rl_warmup_active());
    CHECK(draw(d) != NULL);
    rl_warmup_reset();
}

TEST(a_corner_card_with_failures_stays_until_it_is_tapped_or_escaped)
{
    fresh();
    rl_warmup_begin(50, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    rl_warmup_progress(50, 3);
    rl_warmup_saving();
    rl_warmup_end();
    int32_t d[4];
    CHECK(draw(d) != NULL);
    float cx = (float)d[0] + (float)d[2] / 2, cy = (float)d[1] + (float)d[3] / 2;
    g_now += 600 * SEC;
    CHECK(draw(d) != NULL);
    CHECK(!rl_warmup_tap(5, 1075));
    CHECK(rl_warmup_active());
    CHECK(rl_warmup_tap(cx, cy));
    CHECK(draw(d) == NULL);
    fresh();
    rl_warmup_begin(50, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    rl_warmup_progress(50, 3);
    rl_warmup_saving();
    rl_warmup_end();
    CHECK(draw(d) != NULL);
    CHECK(rl_warmup_key(RL_WARMUP_KEY_ESCAPE));
    CHECK(draw(d) == NULL);
    CHECK_EQ(g_quits, 0);
    rl_warmup_reset();
}

TEST(the_corner_card_does_not_take_a_key_or_tap_meant_for_the_game)
{
    fresh();
    rl_warmup_begin(500, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    int32_t d[4];
    CHECK(draw(d) != NULL);
    CHECK(!rl_warmup_key(RL_WARMUP_KEY_ESCAPE));
    CHECK(!rl_warmup_key(RL_WARMUP_KEY_ENTER));
    CHECK(!rl_warmup_tap((float)d[0] + (float)d[2] / 2, (float)d[1] + (float)d[3] / 2));
    CHECK_EQ(g_quits, 0);
    rl_warmup_reset();
}

TEST(escape_quits_from_the_blocking_screen_and_ends_the_wait)
{
    fresh();
    rl_warmup_begin(500, RL_WARMUP_BLOCKING);
    CHECK(rl_warmup_key(RL_WARMUP_KEY_ESCAPE));
    CHECK_EQ(g_quits, 1);
    rl_warmup_progress(500, 0);
    rl_warmup_saving();
    rl_warmup_end();
    CHECK_EQ(rl_warmup_wait(), RL_WARMUP_QUIT);
    rl_warmup_reset();
}

TEST(the_quit_button_quits_and_a_tap_elsewhere_is_taken_and_ignored)
{
    fresh();
    rl_warmup_begin(500, RL_WARMUP_BLOCKING);
    int32_t d[4];
    CHECK(draw(d) != NULL);
    CHECK(rl_warmup_tap(3, 3));
    CHECK_EQ(g_quits, 0);
    /* The card's bottom right holds the Quit button: scan for the tap that quits. */
    bool quit = false;
    for (float y = 1080; y > 0 && !quit; y -= 8)
        for (float x = 1920; x > 0 && !quit; x -= 8) {
            rl_warmup_tap(x, y);
            quit = g_quits > 0;
        }
    CHECK(quit);
    rl_warmup_reset();
}

TEST(a_blocking_screen_after_a_good_run_waits_a_moment_and_starts)
{
    fresh();
    rl_warmup_begin(40, RL_WARMUP_BLOCKING);
    rl_warmup_progress(40, 0);
    rl_warmup_saving();
    rl_warmup_end();
    g_now += RL_WU_DONE_NS;
    CHECK_EQ(rl_warmup_wait(), RL_WARMUP_START);
    CHECK_EQ(g_quits, 0);
    rl_warmup_game_started();
    CHECK(!rl_warmup_active());
    rl_warmup_reset();
}

TEST(a_blocking_screen_after_failures_waits_for_start_anyway)
{
    fresh();
    rl_warmup_begin(40, RL_WARMUP_BLOCKING);
    rl_warmup_progress(40, 2);
    rl_warmup_saving();
    rl_warmup_end();
    CHECK(rl_warmup_key(RL_WARMUP_KEY_ENTER));
    CHECK_EQ(rl_warmup_wait(), RL_WARMUP_START);
    CHECK_EQ(g_quits, 0);
    rl_warmup_reset();
}

TEST(the_blocking_screen_does_not_quit_while_the_cache_is_saved)
{
    fresh();
    rl_warmup_begin(40, RL_WARMUP_BLOCKING);
    rl_warmup_progress(40, 0);
    rl_warmup_saving();
    CHECK(rl_warmup_key(RL_WARMUP_KEY_ESCAPE));
    CHECK_EQ(g_quits, 0);
    rl_warmup_reset();
}

TEST(the_note_shows_for_the_first_eight_seconds_of_the_corner_card)
{
    fresh();
    rl_warmup_begin(5000, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    rl_warmup_progress(10, 0);
    int32_t d[4];
    CHECK(draw(d) != NULL);
    int32_t with_note = d[3];
    g_now += 9 * SEC;
    rl_warmup_progress(500, 0);
    CHECK(draw(d) != NULL);
    CHECK(d[3] > 0);
    CHECK(with_note > 0);
    rl_warmup_reset();
}

TEST(the_blocking_card_places_its_buttons_by_state)
{
    rl_wu_scene s;
    memset(&s, 0, sizeof s);
    s.mode = RL_WARMUP_BLOCKING;
    s.state = RL_WU_COMPILING;
    s.total = 2077;
    s.done = 1240;
    s.elapsed_ns = 134 * SEC;
    s.eta_s = 91;
    s.dark = true;
    static uint8_t px[1920 * 1080 * 4];
    rl_wu_canvas c = { .px = px, .w = 1920, .h = 1080 };
    rl_wu_hits hits;
    rl_wu_draw(&c, &s, &hits);
    CHECK(hits.quit[2] > 0 && hits.quit[3] > 0);
    CHECK(hits.start[2] == 0);
    CHECK(hits.card[2] > 0 && hits.card[0] + hits.card[2] / 2 > 1920 / 2 - 2 && hits.card[0] + hits.card[2] / 2 < 1920 / 2 + 2);
    s.state = RL_WU_ERROR;
    s.done = s.total;
    s.failed = 12;
    rl_wu_clear(&c);
    rl_wu_draw(&c, &s, &hits);
    CHECK(hits.quit[2] > 0 && hits.start[2] > 0);
    CHECK(hits.start[0] > hits.quit[0]);
    s.state = RL_WU_FINISHING;
    rl_wu_clear(&c);
    rl_wu_draw(&c, &s, &hits);
    CHECK(hits.quit[2] == 0 && hits.start[2] == 0);
}

TEST(queued_compiles_start_a_pass_whose_card_shows_after_a_second)
{
    fresh();
    rl_warmup_game_started();
    int32_t d[4];
    rl_warmup_queue(3);
    rl_warmup_done(1, 0);
    CHECK(draw(d) == NULL);
    g_now += SEC / 2;
    rl_warmup_queue(2);
    CHECK(draw(d) == NULL);
    CHECK(!rl_warmup_window_due());
    g_now += SEC;
    CHECK(draw(d) != NULL);
    rl_warmup_reset();
}

TEST(a_pass_the_queue_started_has_an_open_total_and_ends_when_all_is_done)
{
    fresh();
    rl_warmup_game_started();
    rl_warmup_queue(10);
    g_now += 2 * SEC;
    int32_t d[4];
    CHECK(draw(d) != NULL);
    rl_warmup_done(10, 0);
    g_now += RL_WU_READY_NS / 2;
    CHECK(draw(d) != NULL);
    g_now += 20 * SEC;
    CHECK(draw(d) == NULL);
    CHECK(!rl_warmup_active());
    rl_warmup_reset();
}

TEST(a_short_pass_leaves_nothing_behind_and_the_window_is_asked_for_before_the_game)
{
    fresh();
    rl_warmup_queue(4);
    CHECK(g_base_on);
    g_now += 2 * SEC;
    CHECK(rl_warmup_window_due());
    rl_warmup_game_started();
    CHECK(!rl_warmup_window_due());
    rl_warmup_reset();
    fresh();
    rl_warmup_game_started();
    rl_warmup_queue(4);
    rl_warmup_done(4, 0);
    g_now += 10 * SEC;
    int32_t d[4];
    CHECK(draw(d) == NULL);
    CHECK(!rl_warmup_active());
    rl_warmup_reset();
}

TEST(failed_compiles_of_a_queued_pass_show_as_failures_that_stay)
{
    fresh();
    rl_warmup_game_started();
    rl_warmup_queue(5);
    g_now += 2 * SEC;
    int32_t d[4];
    CHECK(draw(d) != NULL);
    rl_warmup_done(5, 2);
    g_now += 3 * SEC;
    CHECK(draw(d) != NULL);
    g_now += 600 * SEC;
    CHECK(draw(d) != NULL);
    rl_warmup_reset();
}

TEST(a_launch_warm_up_with_late_compiles_beside_it_counts_up_on_the_screen)
{
    fresh();
    rl_warmup_begin(1000, RL_WARMUP_BACKGROUND);
    rl_warmup_game_started();
    g_now += 2 * SEC;
    int32_t d[4];
    CHECK(draw(d) != NULL);
    uint32_t last_done = 0;
    for (int step = 1; step <= 20; step++) {
        /* A draw's late compile queues and finishes one; the warm-up's workers finish fifty. */
        rl_warmup_queue(1);
        rl_warmup_done(1, 0);
        rl_warmup_done(50, 0);
        g_now += SEC / 4;
        CHECK(draw(d) != NULL);
        CHECK(d[2] > 0 && d[3] > 0);
        rl_wu_scene s;
        rl_wu_test_scene(&s);
        CHECK(s.done > last_done);
        CHECK_EQ(s.total, 1000u + (uint32_t)step);
        CHECK_EQ(s.done, (uint32_t)step * 51u);
        last_done = s.done;
    }
    rl_warmup_reset();
}
