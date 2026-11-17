/**
 * @file rsp.c
 * @brief The RSP layer (rsp.h): the context, the binding of lifted images
 * to IMEM, the run loop of a slice and missed-code reports.
 */
#include "rsp.h"

#include <relift/rsp/abi.h>

#include "core.h"
#include "crash.h"
#include "pipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rl_rsp {
    rl_ctx c;
    rl_fn disp[RL_RSP_SLOTS_COUNT];
    uint16_t bases[RL_RSP_SLOTS_COUNT];
    /** The image bound at each slot. */
    uint16_t slot_image[RL_RSP_SLOTS_COUNT];
    uint8_t *dmem;
    const uint8_t *imem;
    const rl_rsp_image *const *images;
    uint32_t count;
    rl_fn **fnmap;
    /** Every word of every image, by its value: where to look for the images that hold an IMEM word. */
    struct place *places;
    uint32_t nplaces;
    const rl_rsp_bus *bus;
    void *user;
    uint64_t until;
    /** A pipeline lifted code gave for a target not bound yet, or at a halt. */
    rl_rsp_pipe_state hold;
    bool held;
    bool leave;
    bool running;
    const char *dump;
    rl_rsp_stats stats;
};

/** @brief A word of an image: its value, the image and the word's index in it. */
struct place {
    uint32_t word;
    uint32_t image;
    uint32_t offset;
};

static rl_rsp *of(rl_ctx *c) { return c->rsp; }

static uint32_t be_word(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int place_order(const void *a, const void *b)
{
    const struct place *x = a, *y = b;
    if (x->word != y->word)
        return x->word < y->word ? -1 : 1;
    if (x->image != y->image)
        return x->image < y->image ? -1 : 1;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

static bool build_places(rl_rsp *r)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < r->count; k++)
        n += r->images[k]->len / 4u;
    r->places = calloc(n ? n : 1, sizeof *r->places);
    if (!r->places)
        return false;
    for (uint32_t k = 0; k < r->count; k++) {
        const rl_rsp_image *im = r->images[k];
        for (uint32_t o = 0; o < im->len / 4u; o++)
            if (r->fnmap[k][o])
                r->places[r->nplaces++] = (struct place){be_word(im->bytes + 4u * o), k, o};
    }
    qsort(r->places, r->nplaces, sizeof *r->places, place_order);
    return true;
}

static uint64_t now(const rl_ctx *c) { return c->time.deadline - (uint64_t)c->time.budget; }

static void end_budget(rl_ctx *c)
{
    c->time.deadline = now(c);
    c->time.budget = 0;
}

static void reset_slots(rl_rsp *r)
{
    for (uint32_t i = 0; i < RL_RSP_SLOTS_COUNT; i++) {
        r->disp[i] = rl_rsp_enter;
        r->bases[i] = 0;
    }
}

/** @brief The IMEM address of offset off of the running image. */
static uint32_t abs_of(const rl_ctx *c, uint64_t off) { return ((uint32_t)off + c->base) & 0xFFCu; }

/** @brief The function of the image at each word offset it lifted, from its unit's entries. */
static rl_fn *build_fnmap(const rl_rsp_image *im)
{
    rl_fn *m = calloc(RL_RSP_SLOTS_COUNT, sizeof *m);
    if (!m)
        return NULL;
    const rl_unit *u = im->unit;
    for (uint32_t k = 0; k < u->fn_count; k++) {
        const rl_fn_entry *e = &u->fns[k];
        uint64_t end = e->guest_addr + ((e->flags & RL_FN_RANGE) ? e->guest_size : 4u);
        for (uint64_t a = e->guest_addr; a < end && a < im->len; a += 4)
            if (!m[a >> 2])
                m[a >> 2] = e->fn;
    }
    return m;
}

bool rl_rsp_images_ok(const rl_rsp_image *const *images, uint32_t count, char *why, size_t n)
{
    for (uint32_t i = 0; i < count; i++) {
        const rl_rsp_image *im = images[i];
        const rl_unit *u = im->unit;
        if (u->abi_version != RL_ABI_VERSION || u->ctx_size != sizeof(rl_ctx)) {
            snprintf(why, n, "RSP unit %s was built for another runtime", u->name ? u->name : "?");
            return false;
        }
        if (im->len % 4 || im->len > RL_RSP_MEM_SIZE) {
            snprintf(why, n, "RSP image %016llx does not fit IMEM", (unsigned long long)im->hash);
            return false;
        }
    }
    return true;
}

rl_rsp *rl_rsp_create(uint8_t *dmem, const uint8_t *imem, const rl_rsp_image *const *images, uint32_t count,
                      const rl_rsp_bus *bus, void *user)
{
    rl_rsp *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->fnmap = calloc(count ? count : 1, sizeof *r->fnmap);
    if (!r->fnmap) {
        free(r);
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        r->fnmap[i] = build_fnmap(images[i]);
        if (!r->fnmap[i]) {
            rl_rsp_destroy(r);
            return NULL;
        }
    }
    r->dmem = dmem;
    r->imem = imem;
    r->images = images;
    r->count = count;
    if (!build_places(r)) {
        rl_rsp_destroy(r);
        return NULL;
    }
    r->bus = bus;
    r->user = user;
    r->c.disp = r->disp;
    r->c.bases = r->bases;
    r->c.rsp = r;
    reset_slots(r);
    for (uint32_t i = 0; i < count; i++) {
        const rl_unit *u = images[i]->unit;
        if (u->map_count)
            rl_fn_maps_register(u->maps, u->map_count);
    }
    return r;
}

void rl_rsp_destroy(rl_rsp *r)
{
    if (!r)
        return;
    if (r->fnmap)
        for (uint32_t i = 0; i < r->count; i++)
            free(r->fnmap[i]);
    free(r->fnmap);
    free(r->places);
    free(r);
}

void rl_rsp_set_dump(rl_rsp *r, const char *prefix) { r->dump = prefix; }
const rl_rsp_stats *rl_rsp_get_stats(const rl_rsp *r) { return &r->stats; }
uint32_t rl_rsp_gpr(const rl_rsp *r, unsigned n) { return n < 32 ? r->c.r[n] : 0; }

void rl_rsp_vpr(const rl_rsp *r, unsigned n, uint8_t out[16])
{
    rl_u128 v = n < 32 ? r->c.v[n] : 0;
    for (int i = 0; i < 16; i++)
        out[i] = (uint8_t)(v >> (8 * (15 - i)));
}

void rl_rsp_imem_written(rl_rsp *r, uint32_t offset, uint32_t len)
{
    (void)offset;
    if (!len)
        return;
    r->stats.invalidations++;
    reset_slots(r);
    if (r->running) {
        r->leave = true;
        end_budget(&r->c);
    }
}

/** @brief Whether IMEM word w holds the word of image im at word offset o. */
static bool word_matches(const rl_rsp *r, const rl_rsp_image *im, uint32_t o, uint32_t w)
{
    if (im->len == RL_RSP_MEM_SIZE)
        o %= RL_RSP_SLOTS_COUNT;
    if (o >= im->len / 4u)
        return false;
    return memcmp(r->imem + (w % RL_RSP_SLOTS_COUNT) * 4u, im->bytes + o * 4u, 4) == 0;
}

/**
 * @brief Checks image k bound with its word offset o at IMEM word w: every
 * block lifted code may reach from o without a computed jump has the
 * image's bytes in IMEM. Binds the slots of those blocks on success.
 */
static bool check(rl_rsp *r, uint32_t k, uint32_t o, uint32_t w)
{
    const rl_rsp_image *im = r->images[k];
    const rl_fn *fm = r->fnmap[k];
    uint32_t base = (w - o) % RL_RSP_SLOTS_COUNT;
    uint32_t words = im->len / 4u;
    uint8_t seen[RL_RSP_SLOTS_COUNT];
    uint16_t todo[RL_RSP_SLOTS_COUNT];
    uint32_t n = 0;
    memset(seen, 0, sizeof seen);
    if (!fm[o])
        return false;
    todo[n++] = (uint16_t)o;
    seen[o] = 1;
    uint32_t done = 0;
    while (done < n) {
        uint32_t i = todo[done++];
        const uint16_t *s = im->succ + 3u * i;
        if (!fm[i] || !word_matches(r, im, i, base + i))
            return false;
        if ((s[2] & RL_RSP_SUCC_SLOT) && !word_matches(r, im, i + 1u, base + i + 1u))
            return false;
        for (int j = 0; j < 2; j++) {
            uint32_t t = s[j];
            if (t == RL_RSP_NO_SUCC || t >= words || seen[t])
                continue;
            seen[t] = 1;
            todo[n++] = (uint16_t)t;
        }
    }
    for (uint32_t j = 0; j < n; j++) {
        uint32_t a = (base + todo[j]) % RL_RSP_SLOTS_COUNT;
        r->disp[a] = fm[todo[j]];
        r->bases[a] = (uint16_t)(base * 4u);
        r->slot_image[a] = (uint16_t)k;
    }
    r->stats.binds++;
    return true;
}

static void dump_mem(const rl_rsp *r)
{
    if (!r->dump)
        return;
    const struct {
        const char *ext;
        const uint8_t *p;
    } parts[2] = {{"imem", r->imem}, {"dmem", r->dmem}};
    for (int i = 0; i < 2; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s.%s", r->dump, parts[i].ext);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(parts[i].p, 1, RL_RSP_MEM_SIZE, f) != RL_RSP_MEM_SIZE)
            rl_log(RL_LOG_ERROR, "rsp", "could not write %s", path);
        else
            rl_log(RL_LOG_ERROR, "rsp", "wrote %s to %s", parts[i].ext, path);
        if (f)
            fclose(f);
    }
}

static uint64_t fnv(const uint8_t *b, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/** @brief Reports missed code at c->pc and stops the program. */
static _Noreturn void missed(rl_rsp *r, const char *why)
{
    rl_ctx *c = &r->c;
    uint32_t pc = abs_of(c, c->pc);
    r->stats.misses++;
    char regs[1024];
    size_t o = 0;
    for (int i = 1; i < 32 && o < sizeof regs; i++)
        o += (size_t)snprintf(regs + o, sizeof regs - o, "%sr%d=%08x", i > 1 ? " " : "", i, c->r[i]);
    rl_log(RL_LOG_ERROR, "rsp",
           "missed RSP code at IMEM 0x%03x (word %08x, IMEM hash %016llx): %s; nothing is interpreted, the program "
           "stops\n  cycle %llu %s",
           pc,
           (uint32_t)r->imem[pc] << 24 | (uint32_t)r->imem[pc + 1] << 16 | (uint32_t)r->imem[pc + 2] << 8 |
               r->imem[pc + 3],
           (unsigned long long)fnv(r->imem, RL_RSP_MEM_SIZE), why, (unsigned long long)now(c), regs);
    dump_mem(r);
    rl_process_exit(RL_EXIT_SOFTWARE);
    abort();
}

/** @brief Charges what bound word w costs from pipeline x more than its block assumes. */
static void pipe_enter(rl_rsp *r, uint32_t w, rl_rsp_pipe_state x)
{
    const rl_rsp_image *im = r->images[r->slot_image[w]];
    if (!im->pipes || !im->pipe_at)
        return;
    uint32_t o = (w - r->bases[w] / 4u) % RL_RSP_SLOTS_COUNT;
    if (o >= im->len / 4u || im->pipe_at[o] == RL_RSP_NO_PIPE)
        return;
    const uint64_t *p = im->pipes + 3u * im->pipe_at[o];
    bool met;
    int64_t d = rl_rsp_pipe_delta(x, rl_rsp_pipe_unpack(p[0], p[1], p[2]), w, r->imem, &met);
    r->stats.pipe_unmet += !met;
    r->c.time.budget -= d;
}

/** @brief Charges a held pipeline at bound word w. */
static void take_hold(rl_rsp *r, uint32_t w)
{
    if (!r->held || r->disp[w] == rl_rsp_enter)
        return;
    r->held = false;
    pipe_enter(r, w, r->hold);
}

void rl_rsp_pipe(rl_ctx *c, uint32_t target, uint64_t a, uint64_t b, uint64_t d, uint32_t taken)
{
    rl_rsp *r = of(c);
    uint32_t w = abs_of(c, target) >> 2;
    rl_rsp_pipe_state x = rl_rsp_pipe_unpack(a, b, d);
    if (taken) {
        rl_rsp_pipe_taken(&x, w);
        c->time.budget -= 1;
    }
    r->hold = x;
    r->held = true;
    take_hold(r, w);
}

void rl_rsp_pipe_hold(rl_ctx *c, uint64_t a, uint64_t b, uint64_t d)
{
    rl_rsp *r = of(c);
    r->hold = rl_rsp_pipe_unpack(a, b, d);
    r->held = true;
}

void rl_rsp_tick(rl_ctx *c, int64_t n) { c->time.budget -= n; }

uint64_t rl_rsp_enter(rl_ctx *c, uint8_t *mem)
{
    rl_rsp *r = of(c);
    uint32_t w = abs_of(c, c->pc) >> 2;
    c->base = 0;
    c->pc = w * 4u;
    uint32_t candidates = 0;
    uint32_t word = be_word(r->imem + w * 4u);
    uint32_t lo = 0, hi = r->nplaces;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (r->places[mid].word < word)
            lo = mid + 1u;
        else
            hi = mid;
    }
    for (uint32_t i = lo; i < r->nplaces && r->places[i].word == word; i++) {
        candidates++;
        if (check(r, r->places[i].image, r->places[i].offset, w)) {
            take_hold(r, w);
            RL_MUSTTAIL return rl_indirect(c, w * 4u)(c, mem);
        }
    }
    char why[160];
    if (candidates)
        snprintf(why, sizeof why, "the bytes there differ from the %u lifted image places that hold the word", candidates);
    else
        snprintf(why, sizeof why, "no lifted image holds the word");
    missed(r, why);
}

int rl_rsp_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    char why[96];
    snprintf(why, sizeof why, "the word %08x did not lift", insn);
    c->pc = pc & 0xFFCu;
    missed(of(c), why);
}

int rl_rsp_sync(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)mem;
    rl_rsp *r = of(c);
    uint64_t t = now(c);
    if (!r->leave && t < r->until && !r->bus->halted(r->user)) {
        c->time.budget = (int64_t)(r->until - t);
        c->time.deadline = r->until;
        return RL_OK;
    }
    c->pc = abs_of(c, next);
    c->base = 0;
    return RL_PENDING;
}

uint32_t rl_rsp_mfc0(rl_ctx *c, uint32_t rd)
{
    rl_rsp *r = of(c);
    return r->bus->mfc0(r->user, rd & 15u, now(c));
}

void rl_rsp_mtc0(rl_ctx *c, uint32_t rd, uint32_t value, uint32_t run)
{
    if (!run)
        return;
    rl_rsp *r = of(c);
    r->bus->mtc0(r->user, rd & 15u, value, now(c));
    end_budget(c);
}

void rl_rsp_break(rl_ctx *c, uint32_t run)
{
    if (!run)
        return;
    rl_rsp *r = of(c);
    r->bus->brk(r->user, now(c));
    end_budget(c);
}

uint64_t rl_rsp_run(rl_rsp *r, uint32_t *pc, uint64_t from, uint64_t to)
{
    rl_ctx *c = &r->c;
    if (to <= from)
        return from;
    r->stats.slices++;
    r->until = to;
    r->running = true;
    c->pc = *pc & 0xFFCu;
    c->base = 0;
    c->time.deadline = to;
    c->time.budget = (int64_t)(to - from);
    take_hold(r, c->pc >> 2);
    for (;;) {
        r->leave = false;
        rl_fn f = rl_indirect(c, c->pc);
        f(c, r->dmem);
        if (r->bus->halted(r->user) || now(c) >= to)
            break;
        c->time.budget = (int64_t)(to - now(c));
        c->time.deadline = to;
    }
    r->running = false;
    c->r[0] = 0;
    uint64_t at = now(c);
    r->stats.cycles += at - from;
    *pc = (uint32_t)c->pc & 0xFFCu;
    return at;
}
