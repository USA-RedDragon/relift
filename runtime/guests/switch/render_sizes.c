/**
 * @file render_sizes.c
 * @brief The picture sizes a Switch game offers for video.render_resolution, by program id and console mode.
 *
 * A list is a fact about the game, measured on a run or read from its data, never a guess: a game with no entry
 * offers none, ignores the setting and shows no row. How relift selects a size is the game's own lever:
 *  - Ocarina of Time (Switch 2), docked: the display is fixed at 2560x1440 in the game and never asked about; its
 *    frame is that times the factor of the graphics scale nn::oe::GetCurrentGraphicsScale reads (am
 *    IGraphicsScalingParametersFunctions 10: code 10 is x1, 20 is x0.8, 30 is x0.667). Measured on the docked
 *    1440p run; handheld not measured, so no handheld list.
 *  - Fire Emblem (Switch 2): the game sizes its targets from gsv DisplaySession 106 (nn::vi2::QueryDisplayBufferMaxResolution):
 *    mode 4 is 1440 lines, 2 is 1080, 1 is 720. Docked answers 4 (console), handheld answers 2 (console); measured at
 *    every setting on both modes.
 */
#include "am.h"
#include "hos.h"
#include "services.h"

#include <relift/settings.h>

#include <stdio.h>
#include <string.h>

/** @brief One size of a game: its line count and the label the settings screen shows. */
typedef struct {
    uint32_t lines;
    const char *label;
} size_entry;

#define OOT_TITLE UINT64_C(0x0400486024dec000)
#define FE_TITLE UINT64_C(0x04007ef01f8ae000)

static const size_entry oot_docked[] = {
    { 1440, "1440 lines (console)" },
    { 1152, "1152 lines" },
    { 960, "960 lines" },
};
static const size_entry fe_docked[] = {
    { 1440, "1440 lines (console)" },
    { 1080, "1080 lines" },
    { 720, "720 lines" },
};
static const size_entry fe_handheld[] = {
    { 1080, "1080 lines (console)" },
    { 720, "720 lines" },
};

#define N(a) (sizeof(a) / sizeof((a)[0]))

/** @brief The program's list for a mode, or NULL. */
static const size_entry *list_for(uint64_t program, bool docked, size_t *n)
{
    if (program == OOT_TITLE && docked) {
        *n = N(oot_docked);
        return oot_docked;
    }
    if (program == FE_TITLE) {
        *n = docked ? N(fe_docked) : N(fe_handheld);
        return docked ? fe_docked : fe_handheld;
    }
    *n = 0;
    return NULL;
}

static const size_entry *current_list(size_t *n) { return list_for(hos.cfg.program_id, rl_am_is_docked(), n); }

size_t rl_switch_render_sizes(rl_render_size *out, size_t max)
{
    size_t n;
    const size_entry *l = current_list(&n);
    if (n > max)
        n = max;
    for (size_t i = 0; i < n; i++) {
        out[i].lines = l[i].lines;
        out[i].label = l[i].label;
        out[i].console = i == 0;
    }
    return n;
}

uint32_t rl_am_display_cap(void)
{
    size_t n;
    const size_entry *l = current_list(&n);
    uint32_t want = rl_render_resolution_lines();
    if (!n || !want || want >= l[0].lines)
        return 0;
    for (size_t i = 1; i < n; i++)
        if (l[i].lines <= want)
            return l[i].lines;
    return l[n - 1].lines;
}

uint32_t rl_am_render_lines(void)
{
    size_t n;
    const size_entry *l = current_list(&n);
    if (!n)
        return 0;
    uint32_t cap = rl_am_display_cap();
    return cap ? cap : l[0].lines;
}

void am_log_render_size(const char *how)
{
    size_t n;
    const size_entry *l = current_list(&n);
    if (!n)
        return;
    char offers[96] = "";
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(offers);
        snprintf(offers + len, sizeof offers - len, "%s%u", i ? (i + 1 == n ? " or " : ", ") : "", l[i].lines);
    }
    rl_log(RL_LOG_INFO, "am", "rendering %u lines (%s); this game offers %s", rl_am_render_lines(), how, offers);
}
