/**
 * @file warmup_int.h
 * @brief Inside the compile screen: the numbers and words it shows (measured counts, the rate and the time left),
 * the screen a state picks, and the drawing of each screen (warmup_draw.c).
 */
#ifndef RELIFT_WARMUP_INT_H
#define RELIFT_WARMUP_INT_H

#include <relift/warmup.h>

#include "touchpad_int.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The seconds of compiling and the shaders done before the screen trusts the average rate. */
#define RL_WU_RATE_SECONDS 5u
#define RL_WU_RATE_SHADERS 20u
/** @brief How long the corner card keeps its "Starting now" note after it first shows (ns). */
#define RL_WU_NOTE_NS (8ull * 1000000000ull)
/** @brief How long the corner card says the graphics are ready (ns), and how long a blocking screen shows its result. */
#define RL_WU_READY_NS (5ull * 1000000000ull)
#define RL_WU_DONE_NS (1500ull * 1000000ull)

/** @brief The state a screen shows. */
enum rl_wu_state {
    /** @brief Compiling, with no rate measured yet. */
    RL_WU_MEASURING = 0,
    /** @brief Compiling at a measured rate. */
    RL_WU_COMPILING = 1,
    /** @brief Every shader is done and the cache is being saved. */
    RL_WU_FINISHING = 2,
    /** @brief Done, and the driver turned some shaders down. */
    RL_WU_ERROR = 3,
    /** @brief Done and all went well. */
    RL_WU_DONE = 4
};

/** @brief Everything one drawing needs, copied from the state under its lock. */
typedef struct rl_wu_scene {
    /** @brief enum rl_warmup_mode. */
    int mode;
    /** @brief enum rl_wu_state. */
    int state;
    uint32_t total, done, failed;
    /** @brief The time since the compile began (to its end once it ended), in nanoseconds. */
    uint64_t elapsed_ns;
    /** @brief The seconds left at the measured average rate, when it is measured (state COMPILING). */
    uint32_t eta_s;
    bool dark, keyboard;
    /** @brief The window has no game picture yet: the corner card is drawn over a plain background of its own. */
    bool fill;
    /** @brief The first submit is held for the compile and the game has not started: the blocking card with the "Starting now" note and no buttons. */
    bool starting;
    /** @brief For the corner card: the "Starting now" note shows. */
    bool note;
    char name[256], gpu[256], report[1100];
    /** @brief The game's icon, RGBA8 rows, or NULL. */
    const uint8_t *icon;
    uint32_t icon_w, icon_h;
} rl_wu_scene;

/** @brief Where the screen put its buttons and card, in view pixels (x, y, width, height); zeros when absent. */
typedef struct rl_wu_hits {
    float quit[4], start[4], card[4];
} rl_wu_hits;

/**
 * @brief The seconds left from the average rate so far, when the screen trusts it: at least RL_WU_RATE_SECONDS of
 * compiling and RL_WU_RATE_SHADERS shaders done (or half of a smaller total), and some left. Rounded up.
 */
bool rl_wu_eta(uint32_t done, uint32_t total, uint64_t elapsed_ns, uint32_t *seconds);
/** @brief "About 40 seconds left", "About 1 minute left", "About 2 minutes left", "About 2 hours left". */
void rl_wu_eta_text(uint32_t seconds, char *out, size_t n);
/** @brief Seconds as "m:ss" (h:mm:ss from an hour on). */
void rl_wu_time_text(uint64_t seconds, char *out, size_t n);
/** @brief A count with thousands commas: "2,077". */
void rl_wu_count_text(uint64_t v, char *out, size_t n);
/** @brief The whole percent of done in total (rounded down; 0 for no total). */
uint32_t rl_wu_percent(uint32_t done, uint32_t total);

/** @brief Replaces the clock the model reads, NULL for the real one (tests). */
void rl_wu_test_clock(uint64_t (*clock)(void));

/** @brief The canvas the screen is drawn on: premultiplied RGBA8, the window's whole size (the controller overlay's). */
typedef rl_tp_canvas rl_wu_canvas;

/** @brief Zeroes the rectangle the canvas drew on (rows x0..x1 of y0..y1) and resets it. */
void rl_wu_clear(rl_wu_canvas *c);
/**
 * @brief Draws the scene (the blocking screen over the whole canvas, or the corner card) and says what it drew on in
 * c's rectangle and where its buttons and card sit in hits.
 */
void rl_wu_draw(rl_wu_canvas *c, const rl_wu_scene *s, rl_wu_hits *hits);
/** @brief Adds a TrueType font ahead of the shipped ones (the bytes stay the caller's): for tests and hosts with their own. */
void rl_wu_add_font(const uint8_t *data, size_t size);
/** @brief Forgets the added fonts (tests). */
void rl_wu_reset_fonts(void);
/** @brief True when a TrueType font draws the text; false when the plain stroke font does. */
bool rl_wu_has_font(void);

#endif
