/**
 * @file arch_arm64.c
 * @brief The Linux layer on aarch64: `svc #0` entry with the number in x8 and
 * the arguments in x0 to x5, the frame the arm64 kernel builds (siginfo,
 * ucontext, and the floating point and SIMD state), a 64-bit initial stack,
 * and a 39-bit guest address space.
 */
#include "arch.h"
#include "arm64.h"

#include <string.h>

const char lx_arch_name[] = "aarch64";
const unsigned lx_arch_word = 8;
const char lx_arch_platform[] = "aarch64";

enum {
    FRAME_INFO = 0,
    FRAME_UC = L_SIGINFO_BYTES,
    UC_STACK = 16,
    UC_SIGMASK = 40,
    UC_FAULT = L_UCONTEXT_MCONTEXT,
    UC_REGS = UC_FAULT + 8,
    UC_SP = UC_REGS + 31 * 8,
    UC_PC = UC_SP + 8,
    UC_PSTATE = UC_PC + 8,
    UC_RESERVED = UC_PSTATE + 16,
    FRAME_BYTES = L_SIGINFO_BYTES + L_UCONTEXT_MCONTEXT + L_SIGCONTEXT_BYTES
};

#define LX_FPSIMD_MAGIC 0x46508001u
#define LX_FPSIMD_SIZE 0x210u

static const rl_guest_desc linux_guest = {
    .name = "linux",
    .page_size = L_PAGE,
    .address_bits = 39,
    .tick_hz = RL_CNTFRQ,
};

int lx_arch_core_init(void) { return rl_core_init(&linux_guest, rl_arm64_cpu()); }

int lx_arch_vm_init(rl_vm *vm)
{
    unsigned bits = 39;
    if (rl_vm_init(vm, bits, 0) != 0) {
        bits = 36;
        if (rl_vm_init(vm, bits, 0) != 0)
            return -1;
    }
    rl_arm64_set_va_bits(bits);
    return 0;
}

void lx_arch_install_hooks(void) { rl_arm64_set_safepoint_hook(lx_safepoint); }

bool lx_arch_trampoline(void) { return rl_arm64_trampoline(); }

void lx_arch_sys_args(const rl_ctx *c, uint32_t *nr, uint64_t a[6])
{
    for (int i = 0; i < 6; i++)
        a[i] = c->x[i];
    *nr = (uint32_t)c->x[8];
}

void lx_arch_sys_result(rl_ctx *c, int64_t r) { c->x[0] = (uint64_t)r; }

void lx_arch_sys_restart(rl_ctx *c, uint32_t nr, const uint64_t a[6])
{
    c->x[0] = a[0];
    c->x[8] = nr;
    rl_clrex(&c->excl);
}

void lx_arch_sys_begin(rl_ctx *c) { rl_clrex(&c->excl); }

int64_t lx_arch_syscall(rl_ctx *c, uint8_t *mem, uint32_t nr, const uint64_t a[6]) { return lx_dispatch(c, mem, nr, a); }

bool lx_arch_traces_path(uint32_t nr) { return nr == NR_openat || nr == NR_newfstatat || nr == NR_faccessat; }

uint64_t lx_arch_trap_bias(void) { return 4; }

int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm)
{
    (void)imm;
    return lx_syscall_entry(c, mem);
}

uint64_t lx_arch_sp(const rl_ctx *c) { return c->sp; }

void lx_arch_set_sp(rl_ctx *c, uint64_t sp) { c->sp = sp; }

uint64_t lx_arch_pc(const rl_ctx *c) { return c->pc; }

void lx_arch_set_pc(rl_ctx *c, uint64_t pc) { c->pc = pc; }

uint64_t lx_arch_after_trap(const rl_ctx *c) { return c->pc + 4; }

size_t lx_arch_aux_extra(const rl_linux_program *p, uint64_t (*out)[2])
{
    out[0][0] = L_AT_HWCAP;
    out[0][1] = p->hwcap;
    out[1][0] = L_AT_HWCAP2;
    out[1][1] = 0;
    out[2][0] = L_AT_MINSIGSTKSZ;
    out[2][1] = L_MINSIGSTKSZ;
    return 3;
}

int lx_arch_map_extra(void) { return 0; }

uint64_t lx_arch_space_top(const rl_vm *vm) { return vm->size; }

uint64_t lx_arch_frame_bytes(void) { return FRAME_BYTES; }

uint64_t lx_arch_frame_place(uint64_t sp) { return (sp - FRAME_BYTES) & ~15ull; }

int lx_arch_frame_build(rl_ctx *c, uint8_t *mem, uint64_t frame, int sig, const lx_pending_sig *p, const lx_sigaction *act, uint64_t resume)
{
    uint8_t *f = mem + frame;
    memset(f, 0, FRAME_BYTES);
    int32_t si[4] = { sig, 0, p->code, 0 };
    memcpy(f + FRAME_INFO, si, 16);
    memcpy(f + FRAME_INFO + 16, &p->pid, 4);
    memcpy(f + FRAME_INFO + 20, &p->uid, 4);
    uint8_t *uc = f + FRAME_UC;
    int32_t ss_flags = lx_cur()->on_altstack ? L_SS_ONSTACK : (lx_cur()->altstack_size ? 0 : L_SS_DISABLE);
    memcpy(uc + UC_STACK, &lx_cur()->altstack_sp, 8);
    memcpy(uc + UC_STACK + 8, &ss_flags, 4);
    memcpy(uc + UC_STACK + 16, &lx_cur()->altstack_size, 8);
    memcpy(uc + UC_SIGMASK, &lx_cur()->blocked, 8);
    memcpy(uc + UC_REGS, c->x, 31 * 8);
    memcpy(uc + UC_SP, &c->sp, 8);
    memcpy(uc + UC_PC, &resume, 8);
    uint32_t hdr[2] = { LX_FPSIMD_MAGIC, LX_FPSIMD_SIZE };
    uint32_t fp[2] = { rl_mrs_fpsr(c), (uint32_t)rl_mrs_fpcr(c) };
    memcpy(uc + UC_RESERVED, hdr, 8);
    memcpy(uc + UC_RESERVED + 8, fp, 8);
    for (int i = 0; i < 32; i++)
        memcpy(uc + UC_RESERVED + 16 + 16 * i, &c->v[i], 16);
    c->x[0] = (uint64_t)sig;
    c->x[1] = frame + FRAME_INFO;
    c->x[2] = frame + FRAME_UC;
    c->x[30] = (act->flags & L_SA_RESTORER) ? act->restorer : 0;
    c->sp = frame;
    return 0;
}

uint64_t lx_arch_frame_restore(rl_ctx *c, uint8_t *mem, uint64_t frame, uint64_t *mask)
{
    uint8_t *uc = mem + frame + FRAME_UC;
    uint64_t new_pc;
    uint32_t fp[2];
    memcpy(&new_pc, uc + UC_PC, 8);
    memcpy(c->x, uc + UC_REGS, 31 * 8);
    memcpy(&c->sp, uc + UC_SP, 8);
    memcpy(fp, uc + UC_RESERVED + 8, 8);
    rl_msr_fpsr(c, fp[0]);
    rl_msr_fpcr(c, fp[1]);
    for (int i = 0; i < 32; i++)
        memcpy(&c->v[i], uc + UC_RESERVED + 16 + 16 * i, 16);
    memcpy(mask, uc + UC_SIGMASK, 8);
    return new_pc;
}

void lx_arch_redirect(rl_ctx *c, uint64_t resume_from) { c->pc = resume_from; }

int lx_arch_clone_regs(rl_ctx *n, const rl_ctx *c, uint64_t flags, const uint64_t a[6])
{
    memcpy(n->x, c->x, sizeof n->x);
    memcpy(n->v, c->v, sizeof n->v);
    n->fpcr = c->fpcr;
    n->fpsr = c->fpsr;
    n->tpidr_el0 = (flags & L_CLONE_SETTLS) ? a[3] : c->tpidr_el0;
    n->x[0] = 0;
    n->x[30] = 0;
    n->sp = a[1] ? a[1] : c->sp;
    return 0;
}

unsigned lx_arch_clone_ctid_arg(void) { return 4; }

void lx_arch_start_regs(rl_ctx *c, uint64_t sp, uint64_t entry)
{
    (void)entry;
    c->sp = sp;
}

int lx_arch_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry) { return rl_call_guest(c, mem, entry); }
