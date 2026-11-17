/* hwtest 3c: main RAM contention between the two CPUs (included by each main.c after hwtest.h and ct.h).
   A case times one CPU's kernel (16 and 48 iterations after 4 warm ones, as the kernel cases) while the
   other CPU runs a background kernel over the same span, or one of its DMA channels copies main RAM to
   main RAM, or it waits on I/O (alone). The ARM9's kernels load and store uncached, unbuffered main RAM or
   run from it uncached; the ARM7's load and store main RAM or run from it. Both CPUs start together on
   IPCSYNC with interrupts off, and the case runs with each setting of EXMEMCNT bit 15 (main RAM priority).
   The background runs about four times the measured CPU's alone time; kaddr counts the runs whose
   background ended before the measured CPU did (0: every run fully overlapped). Rows from idx 4000:
   cpu = the measured CPU, name = its kernel "_vs_" the other's (idle, dma or a kernel), region = p9 (ARM9
   priority) or p7, place = where the measured CPU's code ran. Each main.c includes ct.h and defines the
   hooks first. */

enum { CT_READY = 5, CT_GO, CT_DONE, CT_ACK, CT_BGDONE, CT_QDONE };
#define CT_IDLE 0xFEu
#define CT_DMA 0xFFu
#define CT_SYNC (*(vu16 *)0x04000180)
#define CT_DMA3 ((vu32 *)0x040000D4)

static void ct_out(u32 v) { CT_SYNC = (u16)((CT_SYNC & 0x4000) | v << 8); }
static u32 ct_in(void) { return CT_SYNC & 15; }
static void ct_wait(u32 v)
{
    while (ct_in() != v)
        ;
}

static void ct_dma_start(u32 src, u32 dst, u32 words)
{
    CT_DMA3[2] = 0;
    CT_DMA3[0] = src;
    CT_DMA3[1] = dst;
    CT_DMA3[2] = words | 1u << 26 | 1u << 31;
}

static void ct_dma_wait(void)
{
    while (CT_DMA3[2] & 1u << 31)
        ;
}

/* the measured CPU's part of one run: 4 warm iterations, then 16 and 48 timed; true when the
   background had ended before the timing did */
typedef void ct_kfn(unsigned n);
static bool ct_measured(ct_kfn *k, u32 bg_end, u32 *d16, u32 *d48)
{
    k(4);
    u32 t0 = hw_now();
    k(HW_N1);
    u32 t1 = hw_now();
    k(HW_N2);
    u32 t2 = hw_now();
    bool early = ct_in() == bg_end;
    *d16 = t1 - t0;
    *d48 = t2 - t1;
    return early;
}

#ifdef ARM7
static hw_job7 *ct_job_now;
static void ct_k7_run(unsigned n) { ct_k7(ct_job_now->arg[0], n, ct_job_now); }

/* the ARM7's part of a contention job: HW_REPS runs; measured, it leaves the least times and the count of
   runs whose background ended early in the job */
static void ct_job7(hw_job7 *j)
{
    u32 best[2] = {~0u, ~0u}, early = 0;
    ct_job_now = j;
    ct_prep7(j);
    for (int r = 0; r < HW_REPS; r++) {
        ct_out(CT_READY);
        while (ct_in() != CT_GO && ct_in() != CT_BGDONE)
            ;
        if (j->arg[1]) {
            u32 d16, d48;
            early += ct_measured(ct_k7_run, CT_BGDONE, &d16, &d48);
            if (d16 < best[0])
                best[0] = d16;
            if (d48 < best[1])
                best[1] = d48;
        } else if (j->arg[0] == CT_DMA) {
            ct_dma_start(j->arg[3], j->arg[3] + 0x8000u, j->arg[2]);
            ct_dma_wait();
        } else if (j->arg[0] != CT_IDLE) {
            ct_k7(j->arg[0], j->arg[2], j);
        }
        ct_out(CT_DONE);
        ct_wait(CT_ACK);
    }
    ct_out(0);
    j->t16 = best[0];
    j->t48 = best[1];
    j->kaddr = early;
    j->dbase = 32;
}
#endif

#ifdef ARM9
/* a / b by shifts and subtractions (neither build may link a division routine) */
static u32 ct_udiv(u32 a, u32 b)
{
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = r << 1 | (a >> i & 1);
        if (r >= b) {
            r -= b;
            q |= 1u << i;
        }
    }
    return q;
}

/* the background count: about four times the measured kernel's alone time (ticks x100 per iteration
   ma) in background iterations of ba ticks x100 each */
static u32 ct_bg_count(u32 ma, u32 ba)
{
    u32 span = 4u * (4 + HW_N1 + HW_N2) * ma + 200000u;
    return ct_udiv(span, ba ? ba : 100u) + 16u;
}

/* the ARM9 kernels' alone times, then the ARM7's (ticks x100 per iteration), from the alone cases */
static u32 ct_alone9[CT9_N], ct_alone7[CT7_N];
static unsigned ct_k9_now;
static void ct_k9_run(unsigned n) { ct_k9(ct_k9_now, n); }

static const char *const ct_bg_name[] = {"idle", "dma"};

static void ct_name(char *out, const char *a, const char *b)
{
    while (*a)
        *out++ = *a++;
    for (const char *v = "_vs_"; *v;)
        *out++ = *v++;
    while (*b)
        *out++ = *b++;
    *out = 0;
}

/* one case: measured CPU meas, the ARM9's kernel k9 and the ARM7's k7 (or CT_IDLE, CT_DMA), priority to
   the ARM7 when p7 */
static void ct_case(unsigned idx, int meas, unsigned k9, unsigned k7, bool p7, u8 *dmabuf)
{
    vu16 *exmem = (vu16 *)0x04000204;
    u16 exmem_old = *exmem;
    *exmem = (u16)((exmem_old & 0x7FFF) | (p7 ? 0x8000 : 0));
    u32 bgc = 0;
    if (meas == 9 && k7 < CT7_N)
        bgc = ct_bg_count(ct_alone9[k9], ct_alone7[k7]);
    else if (meas == 9 && k7 == CT_DMA)
        bgc = ct_bg_count(ct_alone9[k9], 400u);
    else if (meas == 7 && k9 < CT9_N)
        bgc = ct_bg_count(ct_alone7[k7], ct_alone9[k9]);
    else if (meas == 7 && k9 == CT_DMA)
        bgc = ct_bg_count(ct_alone7[k7], 400u);
    if (bgc > 8000u && (k7 == CT_DMA || k9 == CT_DMA))
        bgc = 8000u;
    hw_job7 *j = ct_send7(k7, meas == 7, bgc, (u32)dmabuf);
    if (k9 < CT9_N)
        ct_prep9(k9);
    ct_k9_now = k9;
    u32 ime = *(vu32 *)0x04000208;
    *(vu32 *)0x04000208 = 0;
    u32 best[2] = {~0u, ~0u}, early = 0;
    for (int r = 0; r < HW_REPS; r++) {
        ct_wait(CT_READY);
        ct_out(CT_GO);
        if (meas == 9) {
            u32 d16, d48;
            early += ct_measured(ct_k9_run, CT_DONE, &d16, &d48);
            if (d16 < best[0])
                best[0] = d16;
            if (d48 < best[1])
                best[1] = d48;
        } else {
            if (k9 == CT_DMA) {
                ct_dma_start((u32)dmabuf, (u32)dmabuf + 0x8000u, bgc);
                ct_dma_wait();
            } else if (k9 != CT_IDLE) {
                ct_k9(k9, bgc);
            }
            ct_out(CT_BGDONE);
        }
        ct_wait(CT_DONE);
        ct_out(CT_ACK);
    }
    if (k9 < CT9_N)
        ct_unprep9();
    *(vu32 *)0x04000208 = ime;
    ct_finish7(j);
    if (meas == 7) {
        best[0] = j->t16;
        best[1] = j->t48;
        early = j->kaddr;
    }
    *exmem = exmem_old;
    char name[32];
    const char *n9 = k9 < CT9_N ? ct9_name[k9] : ct_bg_name[k9 == CT_DMA];
    const char *n7 = k7 < CT7_N ? ct7_name[k7] : ct_bg_name[k7 == CT_DMA];
    ct_name(name, meas == 9 ? n9 : n7, meas == 9 ? n7 : n9);
    bool code = meas == 9 ? ct9_code[k9] : ct7_code[k7];
    extra(meas, idx, name, p7 ? "p7" : "p9", code ? (meas == 9 ? "unc" : "main") : (meas == 9 ? "main" : "wram"),
          best[0], best[1], early, HW_N2 - HW_N1);
    u32 per = ct_udiv((best[1] - best[0]) * 100u, HW_N2 - HW_N1);
    if (meas == 9 && k7 == CT_IDLE)
        ct_alone9[k9] = per;
    if (meas == 7 && k9 == CT_IDLE)
        ct_alone7[k7] = per;
}

/* every contention case: alone, against each kernel of the other CPU, against its DMA; each priority */
static void ct_all(u8 *dmabuf)
{
    unsigned idx = 4000;
    for (int p = 0; p < 2; p++) {
        for (unsigned a = 0; a < CT9_N; a++)
            ct_case(idx++, 9, a, CT_IDLE, p, dmabuf);
        for (unsigned b = 0; b < CT7_N; b++)
            ct_case(idx++, 7, CT_IDLE, b, p, dmabuf);
        for (unsigned a = 0; a < CT9_N; a++) {
            for (unsigned b = 0; b < CT7_N; b++)
                ct_case(idx++, 9, a, b, p, dmabuf);
            ct_case(idx++, 9, a, CT_DMA, p, dmabuf);
        }
        for (unsigned b = 0; b < CT7_N; b++) {
            for (unsigned a = 0; a < CT9_N; a++)
                ct_case(idx++, 7, a, b, p, dmabuf);
            ct_case(idx++, 7, CT_DMA, b, p, dmabuf);
        }
    }
}
#endif
