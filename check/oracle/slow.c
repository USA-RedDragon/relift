/*
 * The slow-path engine: runs one A64 FP instruction through the runtime's
 * FPCR slow path (rl_fp_slow, runtime/cpus/arm64/fp_slow.c and fp.c) and
 * reports the state after it, in the oracle protocol (proto.h). The checker
 * runs a block that left through its FPCR guard here and compares the
 * result with the oracle, so the slow paths are checked against hardware
 * for every FPCR mode. Host-native: build with check/oracle/build-slow.sh.
 */
#include "proto.h"

#include <stdarg.h>

#include <relift/arm64/abi.h>
#include <relift/arm64/fp_slow.h>

int rl_arm64_fp_flags(void);

static int g_fatal;

/* The runtime's fatal report, for the one caller fp_slow.c has. */
int rl_guest_fatal(rl_ctx *c, const char *tag, const char *fmt, ...)
{
    (void)c;
    (void)tag;
    (void)fmt;
    g_fatal = 1;
    return RL_PENDING;
}

/* Flags go to the context here, never to the host. */
int rl_arm64_fp_flags(void) { return 0; }
int rl_arm64_fp_flags_on;

static uint8_t g_window[DATA_SIZE], g_mem[DATA_SIZE];

int main(void)
{
    static rl_ctx c;
    for (;;) {
        uint32_t words[MAX_WORDS];
        unsigned n;
        gstate in, out;
        if (!proto_read(words, &n, &in, g_window))
            break;
        memcpy(g_mem, g_window, DATA_SIZE);
        memset(&c, 0, sizeof c);
        memcpy(c.x, in.x, sizeof c.x);
        c.sp = in.sp;
        memcpy(c.v, in.v, sizeof c.v);
        rl_set_nzcv(&c, in.nzcv);
        c.fpcr = in.fpcr;
        c.fpsr = in.fpsr;
        g_fatal = 0;
        uint64_t pc = CODE + 4 * (n - 1);
        uint32_t status = ST_DONE;
        if (n != 1) {
            status = ST_UNSUPPORTED;
        } else {
            int r = rl_fp_slow(&c, g_mem, pc, words[0]);
            if (r != RL_OK || g_fatal)
                status = ST_UNSUPPORTED;
        }
        memset(&out, 0, sizeof out);
        memcpy(out.x, c.x, sizeof out.x);
        out.sp = c.sp;
        out.pc = pc + 4;
        out.nzcv = rl_nzcv(&c);
        out.fpcr = c.fpcr;
        out.fpsr = c.fpsr;
        memcpy(out.v, c.v, sizeof out.v);
        proto_write(status, 0, 0, &out, g_mem, g_window);
    }
    return 0;
}
