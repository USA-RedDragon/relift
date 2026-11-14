/**
 * @file crash_win.c
 * @brief The crash handler on Windows: a vectored exception handler for
 * access violations, illegal instructions and in-page errors, the host
 * backtrace from the unwind tables (RtlVirtualUnwind), and module names.
 */
#if defined(_WIN32)

#include "crash.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int g_installed;
static volatile LONG g_reporting;

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
    default: return NULL;
    }
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char *kind = kind_of(code);
    if (!kind)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_reporting, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    rl_crash_info ci;
    memset(&ci, 0, sizeof ci);
    ci.kind = kind;
    ci.pc = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        ci.has_addr = true;
        ci.addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    }
    ci.nframes = walk(ep->ContextRecord, ci.frames, RL_CRASH_MAX_FRAMES);
    rl_crash_report(&ci);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH;
}

void rl_crash_install(void)
{
    if (g_installed)
        return;
    g_installed = 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
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
