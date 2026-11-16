/**
 * @file crash_win.c
 * @brief The crash handler on Windows: a vectored exception handler for
 * access violations, illegal instructions, in-page errors and stack
 * overflows, the host backtrace from the unwind tables (RtlVirtualUnwind),
 * and module names. A stack overflow is reported from a fiber of its own,
 * as the overflowed stack has only the guarantee left.
 */
#if defined(_WIN32)

#include "crash.h"
#include "fiber.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int g_installed;
static volatile LONG g_reporting;
static rl_fiber g_report_fiber;
static rl_fiber g_overflowed;
static EXCEPTION_POINTERS *g_overflow_ep;

/** @brief Host stack of the stack overflow report. */
#define REPORT_STACK (256u << 10)

#if defined(_M_X64) || defined(__x86_64__)
#define CTX_PC(c) ((c)->Rip)
#define CTX_SP(c) ((c)->Rsp)
#else
#define CTX_PC(c) ((c)->Pc)
#define CTX_SP(c) ((c)->Sp)
#endif

static int walk(const CONTEXT *start, uintptr_t *frames, int max)
{
    CONTEXT c = *start;
    int n = 0;
    while (n < max) {
        DWORD64 pc = CTX_PC(&c);
        if (!pc)
            break;
        frames[n++] = (uintptr_t)pc;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(n == 1 ? pc : pc - 1, &image, NULL);
        DWORD64 sp = CTX_SP(&c);
        if (!f) {
            CTX_PC(&c) = *(DWORD64 *)(uintptr_t)sp;
            CTX_SP(&c) = sp + 8;
        } else {
            PVOID data = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, f, &c, &data, &frame, NULL);
        }
        if (CTX_SP(&c) <= sp)
            break;
    }
    return n;
}

static const char *kind_of(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    default: return NULL;
    }
}

static void report(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    rl_crash_info ci;
    memset(&ci, 0, sizeof ci);
    ci.kind = kind_of(code);
    ci.pc = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    ci.sp = (uintptr_t)CTX_SP(ep->ContextRecord);
    ci.stack_overflow = code == EXCEPTION_STACK_OVERFLOW;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        ci.has_addr = true;
        ci.addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    }
    ci.nframes = walk(ep->ContextRecord, ci.frames, RL_CRASH_MAX_FRAMES);
    rl_crash_report(&ci);
    TerminateProcess(GetCurrentProcess(), code);
}

static void report_main(void *arg)
{
    (void)arg;
    report(g_overflow_ep);
    for (;;)
        TerminateProcess(GetCurrentProcess(), EXCEPTION_STACK_OVERFLOW);
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!kind_of(code))
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_reporting, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW && g_report_fiber.sp) {
        g_overflow_ep = ep;
        rl_fiber_switch(&g_overflowed, &g_report_fiber);
    }
    report(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void rl_crash_install(void)
{
    if (g_installed)
        return;
    g_installed = 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (rl_fiber_init(&g_report_fiber, REPORT_STACK, report_main, NULL) == 0)
        snprintf(g_report_fiber.label, sizeof g_report_fiber.label, "the crash report");
    rl_fiber_thread_init();
    AddVectoredExceptionHandler(1, handler);
}

void rl_crash_signal(int sig, void *siginfo, void *ucontext)
{
    (void)sig;
    (void)siginfo;
    (void)ucontext;
}

bool rl_crash_os_symbolize(uintptr_t pc, char *buf, size_t n)
{
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)pc, &m) || !m)
        return false;
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(m, path, sizeof path);
    if (!len)
        return false;
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    snprintf(buf, n, "%s+0x%llx", base, (unsigned long long)(pc - (uintptr_t)m));
    return true;
}

__attribute__((noinline)) int rl_crash_capture(uintptr_t *frames, int max)
{
    PVOID tmp[RL_CRASH_MAX_FRAMES];
    if (max > RL_CRASH_MAX_FRAMES)
        max = RL_CRASH_MAX_FRAMES;
    USHORT n = RtlCaptureStackBackTrace(1, (DWORD)max, tmp, NULL);
    for (USHORT i = 0; i < n; i++)
        frames[i] = (uintptr_t)tmp[i];
    return n;
}

void rl_crash_os_write(const char *s, size_t n)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, s, (DWORD)n, &w, NULL) || !w)
            return;
        s += w;
        n -= w;
    }
}

#endif
