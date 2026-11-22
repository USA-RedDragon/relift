/**
 * @file vi_clock.c
 * @brief The vi vsync timeline and its lock to the host display.
 *
 * The timeline is a 60 Hz grid of CLOCK_MONOTONIC times. Left alone it
 * drifts against the host display, whose refresh is never exactly 60 Hz
 * (68 ppm on an iPhone 13 Pro, 1000 ppm on a 59.94 Hz panel): a frame is
 * then shown twice or dropped every 1/(drift) frames, and the latency
 * between the two sweeps over a whole frame in between. When the host
 * reports when it showed a present (vi_clock_flip), the grid's phase is
 * steered toward the host's refresh, a few microseconds a frame, so a vi
 * vsync falls on a host refresh and the guest renders at the host's rate.
 * A host whose refresh is not near a multiple of 60 Hz (75 or 144 Hz) gives
 * flip times that do not line up from frame to frame; the grid is steered
 * only after 64 flips in a row mostly line up, so there it runs free. A
 * host that shows each present a fixed time after it arrives, with no
 * refresh of its own (a compositor that draws on demand), gives flips that
 * line up too but move with the grid: steering never closes the gap there,
 * and the grid is left alone for a while when a window of steering did not
 * shrink it. The guest's counter (CNTPCT) is not touched by any of this.
 */
#include "vi.h"

#define WILD_NS 2000000ll
#define SLEW_NS 50000ll
#define WINDOW 64u
#define WILD_MAX 16u
#define RUNAWAY_NS 20000ll
#define BACKOFF (WINDOW * 50u)

void vi_clock_init(vi_clock *c, int64_t epoch, bool lock)
{
    *c = (vi_clock){ .epoch = epoch, .lock = lock };
}

int64_t vi_clock_after(const vi_clock *c, int64_t t)
{
    if (t < c->epoch)
        return c->epoch - ((c->epoch - t - 1) / VI_VSYNC_NS) * VI_VSYNC_NS;
    return c->epoch + ((t - c->epoch) / VI_VSYNC_NS + 1) * VI_VSYNC_NS;
}

int64_t vi_clock_index(const vi_clock *c, int64_t t)
{
    int64_t d = t - c->epoch;
    int64_t k = (d + (d < 0 ? -VI_VSYNC_NS / 2 : VI_VSYNC_NS / 2)) / VI_VSYNC_NS;
    return k;
}

static int64_t wrap(int64_t d)
{
    d %= VI_VSYNC_NS;
    if (d >= VI_VSYNC_NS / 2)
        d -= VI_VSYNC_NS;
    if (d < -VI_VSYNC_NS / 2)
        d += VI_VSYNC_NS;
    return d;
}

int64_t vi_clock_flip(vi_clock *c, int64_t t)
{
    c->flips++;
    int64_t err = wrap(t - c->epoch);
    if (!c->have_phase) {
        c->phase = err;
        c->have_phase = true;
        return 0;
    }
    int64_t d = wrap(err - c->phase);
    if (d > WILD_NS || d < -WILD_NS)
        c->wild++;
    else
        c->phase += d / 8;
    if (++c->seen == WINDOW) {
        int64_t now = c->phase < 0 ? -c->phase : c->phase, then = c->window_phase < 0 ? -c->window_phase : c->window_phase;
        if (c->window_moved >= RUNAWAY_NS * WINDOW && now > then - 500000)
            c->free_until = c->flips + BACKOFF;
        c->steady = c->wild <= WILD_MAX && c->flips >= c->free_until;
        c->seen = c->wild = 0;
        c->window_moved = 0;
        c->window_phase = c->phase;
    }
    if (!c->lock || !c->steady)
        return 0;
    int64_t shift = c->phase / 16;
    if (shift > SLEW_NS)
        shift = SLEW_NS;
    if (shift < -SLEW_NS)
        shift = -SLEW_NS;
    c->epoch += shift;
    c->phase -= shift;
    c->window_moved += shift < 0 ? -shift : shift;
    return shift;
}

bool vi_clock_locked(const vi_clock *c) { return c->lock && c->steady; }
