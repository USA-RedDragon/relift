/**
 * @file test_linux.c
 * @brief The Linux layer over the core and the A64 layer: the initial stack,
 * system call argument and result mapping, errno values, guest memory calls,
 * signal state and process exit, all through rl_svc as a guest's `svc #0`
 * reaches it.
 */
#include "hostio.h"
#include "linux_priv.h"
#include "runner.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static const rl_linux_segment segments[] = {
    { 0x400000, L_PAGE, 0, L_PAGE, RL_LINUX_RX },
    { 0x401000, 2 * L_PAGE, L_PAGE, L_PAGE, RL_LINUX_RW },
};
static uint8_t image[2 * L_PAGE];
static const uint8_t image_id[32] = { 0 };
static const uint8_t conf_bytes[] = "alias a b\n";
static const char *const conf_paths[] = { "/lib/gconv/conf", "/lib/gconv/real/conf" };
static const char *const sub_paths[] = { "/lib/gconv/sub/a.conf" };
static const rl_linux_module modules[] = {
    {
        .name = "prog",
        .role = RL_LINUX_MAIN,
        .file = image,
        .file_size = sizeof image,
        .segments = segments,
        .segment_count = 2,
        .base = 0x400000,
        .span = 3 * L_PAGE,
        .entry = 0x400000,
        .phdr = 0x400040,
        .phent = 56,
        .phnum = 3,
    },
    { .name = "data", .role = RL_LINUX_DATA, .paths = conf_paths, .path_count = 2, .file = conf_bytes, .file_size = sizeof conf_bytes - 1 },
    { .name = "data", .role = RL_LINUX_DATA, .paths = sub_paths, .path_count = 1, .file = conf_bytes, .file_size = 3 },
};
static rl_linux_program prog = {
    .name = "prog",
    .exec_path = "/bin/prog",
    .modules = modules,
    .module_count = 3,
    .code_lo = 0x400000,
    .code_hi = 0x403000,
    .stack_size = 0x800000,
    .hwcap = 0xfb,
    .image_id = image_id,
};

static char *g_args[] = { "prog", "one", "two words" };

static rl_ctx *boot(void)
{
    memset(image, 0, sizeof image);
    image[0] = 1;
    image[L_PAGE] = 2;
    rl_ctx *c = NULL;
    if (lx_start(&prog, 3, g_args, &c) != 0)
        return NULL;
    return c;
}

static int64_t sys(rl_ctx *c, uint32_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    c->x[4] = a4;
    c->x[5] = a5;
    c->x[8] = nr;
    int rc = rl_svc(c, rl_mem_base(), 0);
    return rc == RL_PENDING ? INT64_MIN : (int64_t)c->x[0];
}

static uint64_t rd64(uint64_t a) { return rl_ld64(rl_mem_base(), a); }

static const char *cstr(uint64_t a) { return (const char *)(rl_mem_base() + a); }

static uint64_t put_str(uint64_t at, const char *s)
{
    rl_vm_kernel_write(&lx.vm, at, s, strlen(s) + 1);
    return at;
}

TEST(program_files_open_by_path_and_directories_list_what_lies_under_them)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t at = 0x401000;
    put_str(at, "/lib/./gconv/conf");
    int64_t fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0);
    CHECK(fd >= 3);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fd, 0x401100, 64, 0, 0, 0), 10);
    CHECK(memcmp(cstr(0x401100), "alias a b\n", 10) == 0);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fd, 0x401100, 64, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_lseek, (uint64_t)fd, 6, 0, 0, 0, 0), 6);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fd, 0x401100, 64, 0, 0, 0), 4);
    CHECK_EQ(sys(c, NR_fstat, (uint64_t)fd, 0x401200, 0, 0, 0, 0), 0);
    l_stat st;
    memcpy(&st, cstr(0x401200), sizeof st);
    CHECK_EQ(st.st_size, 10);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fd, 0x401100, 64, 0, 0, 0), -L_EBADF);
    put_str(at, "/lib/gconv/real/conf");
    fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0);
    CHECK(fd >= 3);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
    put_str(at, "/lib/gconv");
    fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
    CHECK(fd >= 3);
    int64_t n = sys(c, NR_getdents64, (uint64_t)fd, 0x402000, 1024, 0, 0, 0);
    CHECK(n > 0);
    char names[8][32];
    int count = 0;
    for (int64_t off = 0; off < n && count < 8;) {
        uint16_t reclen;
        memcpy(&reclen, cstr(0x402000 + (uint64_t)off + 16), 2);
        strcpy(names[count++], cstr(0x402000 + (uint64_t)off + 19));
        off += reclen;
    }
    CHECK_EQ(count, 5);
    CHECK(strcmp(names[0], ".") == 0 && strcmp(names[1], "..") == 0);
    CHECK(strcmp(names[2], "conf") == 0 && strcmp(names[3], "real") == 0 && strcmp(names[4], "sub") == 0);
    CHECK_EQ(sys(c, NR_getdents64, (uint64_t)fd, 0x402000, 1024, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
    put_str(at, "/lib/gconv/missing");
    CHECK_EQ(sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0), -L_ENOENT);
}

TEST(initial_stack_holds_argc_argv_envp_and_auxv)
{
    setenv("RELIFT_LX_TEST", "value", 1);
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t sp = c->sp;
    CHECK_EQ(sp % 16, 0);
    CHECK_EQ(rd64(sp), 3);
    CHECK(strcmp(cstr(rd64(sp + 8)), "prog") == 0);
    CHECK(strcmp(cstr(rd64(sp + 16)), "one") == 0);
    CHECK(strcmp(cstr(rd64(sp + 24)), "two words") == 0);
    CHECK_EQ(rd64(sp + 32), 0);
    uint64_t env = sp + 40;
    bool found = false;
    while (rd64(env)) {
        if (strcmp(cstr(rd64(env)), "RELIFT_LX_TEST=value") == 0)
            found = true;
        env += 8;
    }
    CHECK(found);
    uint64_t aux = env + 8;
    uint64_t pagesz = 0, entry = 0, phdr = 0, phnum = 0, phent = 0, hwcap = 0, random = 0, uid = 99, euid = 98, secure = 99;
    int n = 0;
    for (; rd64(aux); aux += 16, n++) {
        uint64_t v = rd64(aux + 8);
        switch (rd64(aux)) {
        case L_AT_PAGESZ: pagesz = v; break;
        case L_AT_ENTRY: entry = v; break;
        case L_AT_PHDR: phdr = v; break;
        case L_AT_PHNUM: phnum = v; break;
        case L_AT_PHENT: phent = v; break;
        case L_AT_HWCAP: hwcap = v; break;
        case L_AT_RANDOM: random = v; break;
        case L_AT_UID: uid = v; break;
        case L_AT_EUID: euid = v; break;
        case L_AT_SECURE: secure = v; break;
        default: break;
        }
    }
    CHECK(n >= 14);
    CHECK_EQ(rd64(aux + 8), 0);
    CHECK_EQ(pagesz, 4096);
    CHECK_EQ(entry, 0x400000);
    CHECK_EQ(phdr, 0x400040);
    CHECK_EQ(phnum, 3);
    CHECK_EQ(phent, 56);
    CHECK_EQ(hwcap, 0xfb);
    CHECK_EQ(secure, 0);
    CHECK_EQ(uid, euid);
    CHECK(lx_ok(random, 16, RL_VM_R));
    CHECK(lx_ok(sp, lx.stack_hi - sp, RL_VM_W));
    CHECK(!lx_ok(lx.stack_lo - 1, 1, RL_VM_R));
}

TEST(the_program_loads_with_its_segment_permissions_and_zero_bss)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK(lx_ok(0x400000, L_PAGE, RL_VM_R | RL_VM_X));
    CHECK(!lx_ok(0x400000, 1, RL_VM_W));
    CHECK(lx_ok(0x401000, 2 * L_PAGE, RL_VM_W));
    CHECK(!lx_ok(0x401000, 1, RL_VM_X));
    CHECK_EQ(rl_ld8(rl_mem_base(), 0x400000), 1);
    CHECK_EQ(rl_ld8(rl_mem_base(), 0x401000), 2);
    CHECK_EQ(rl_ld8(rl_mem_base(), 0x402000), 0);
    CHECK(lx.brk_start >= 0x403000);
}

TEST(unknown_system_calls_fail_with_enosys_and_errors_are_negated_linux_values)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(sys(c, 9999, 0, 0, 0, 0, 0, 0), -L_ENOSYS);
    CHECK_EQ(sys(c, NR_read, 77, 0x401000, 1, 0, 0, 0), -L_EBADF);
    CHECK_EQ(sys(c, NR_close, 77, 0, 0, 0, 0, 0), -L_EBADF);
    put_str(0x401000, "/nonexistent/relift-lx");
    CHECK_EQ(sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, 0x401000, L_O_RDONLY, 0, 0, 0), -L_ENOENT);
    CHECK_EQ(sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, 0x10, L_O_RDONLY, 0, 0, 0), -L_EFAULT);
    CHECK_EQ(sys(c, NR_write, 1, 0x10, 4, 0, 0, 0), -L_EFAULT);
    CHECK_EQ(sys(c, NR_lseek, 1, 0, 7, 0, 0, 0) < 0, 1);
    CHECK_EQ(sys(c, NR_mmap, 0, 0, L_PROT_READ, L_MAP_PRIVATE | L_MAP_ANONYMOUS, (uint64_t)-1, 0), -L_EINVAL);
    CHECK_EQ(sys(c, NR_unlinkat, (uint64_t)(int64_t)L_AT_FDCWD, 0x401000, 0, 0, 0, 0), -L_ENOENT);
}

TEST(host_errno_values_map_to_linux_values)
{
    CHECK_EQ(lx_errno(ENOENT), 2);
    CHECK_EQ(lx_errno(EAGAIN), 11);
    CHECK_EQ(lx_errno(EEXIST), 17);
    CHECK_EQ(lx_errno(ENOTDIR), 20);
    CHECK_EQ(lx_errno(EINVAL), 22);
    CHECK_EQ(lx_errno(ERANGE), 34);
    CHECK_EQ(lx_errno(ENOSYS), 38);
    CHECK_EQ(lx_errno(ENOTEMPTY), 39);
    CHECK_EQ(lx_errno(ELOOP), 40);
    CHECK_EQ(lx_errno(EPIPE), 32);
    CHECK_EQ(lx_errno(ETIMEDOUT), 110);
    CHECK_EQ(lx_errno(ENAMETOOLONG), 36);
}

TEST(pipes_carry_bytes_through_read_write_readv_and_writev)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(sys(c, NR_pipe2, 0x401000, 0, 0, 0, 0, 0), 0);
    int32_t fds[2];
    memcpy(fds, rl_mem_base() + 0x401000, 8);
    CHECK(fds[0] >= 3 && fds[1] >= 3 && fds[0] != fds[1]);
    put_str(0x401100, "hello");
    CHECK_EQ(sys(c, NR_write, (uint64_t)fds[1], 0x401100, 5, 0, 0, 0), 5);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fds[0], 0x401200, 16, 0, 0, 0), 5);
    CHECK(memcmp(rl_mem_base() + 0x401200, "hello", 5) == 0);
    uint64_t iov[4] = { 0x401100, 2, 0x401103, 2 };
    rl_vm_kernel_write(&lx.vm, 0x401300, iov, sizeof iov);
    CHECK_EQ(sys(c, NR_writev, (uint64_t)fds[1], 0x401300, 2, 0, 0, 0), 4);
    uint64_t riov[4] = { 0x401400, 3, 0x401410, 8 };
    rl_vm_kernel_write(&lx.vm, 0x401320, riov, sizeof riov);
    CHECK_EQ(sys(c, NR_readv, (uint64_t)fds[0], 0x401320, 2, 0, 0, 0), 4);
    CHECK(memcmp(rl_mem_base() + 0x401400, "hel", 3) == 0);
    CHECK(rl_ld8(rl_mem_base(), 0x401410) == 'o');
    CHECK_EQ(sys(c, NR_close, (uint64_t)fds[0], 0, 0, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fds[0], 0, 0, 0, 0, 0), -L_EBADF);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fds[1], 0, 0, 0, 0, 0), 0);
}

TEST(fstat_reports_the_aarch64_layout)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(sys(c, NR_pipe2, 0x401000, 0, 0, 0, 0, 0), 0);
    int32_t fds[2];
    memcpy(fds, rl_mem_base() + 0x401000, 8);
    CHECK_EQ(sys(c, NR_fstat, (uint64_t)fds[0], 0x401100, 0, 0, 0, 0), 0);
    l_stat st;
    memcpy(&st, rl_mem_base() + 0x401100, sizeof st);
    CHECK_EQ(st.st_mode & L_S_IFMT, L_S_IFIFO);
    CHECK_EQ(offsetof(l_stat, st_mode), 16);
    CHECK_EQ(offsetof(l_stat, st_size), 48);
    CHECK_EQ(offsetof(l_stat, st_blksize), 56);
    CHECK_EQ(offsetof(l_stat, st_atime_sec), 72);
}

TEST(mmap_brk_mprotect_and_munmap_follow_linux)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    int64_t start = sys(c, NR_brk, 0, 0, 0, 0, 0, 0);
    CHECK_EQ((uint64_t)start, lx.brk_start);
    CHECK_EQ(sys(c, NR_brk, (uint64_t)start + 100, 0, 0, 0, 0, 0), start + 100);
    CHECK(lx_ok((uint64_t)start, 100, RL_VM_W));
    CHECK_EQ(sys(c, NR_brk, (uint64_t)start - 1, 0, 0, 0, 0, 0), start + 100);
    int64_t a = sys(c, NR_mmap, 0, 3 * L_PAGE, L_PROT_READ | L_PROT_WRITE, L_MAP_PRIVATE | L_MAP_ANONYMOUS, (uint64_t)-1, 0);
    CHECK(a > 0 && (a & 0xfff) == 0);
    CHECK(lx_ok((uint64_t)a, 3 * L_PAGE, RL_VM_W));
    CHECK_EQ(rl_ld8(rl_mem_base(), (uint64_t)a + 5000), 0);
    int64_t b = sys(c, NR_mmap, 0, L_PAGE, L_PROT_READ, L_MAP_PRIVATE | L_MAP_ANONYMOUS, (uint64_t)-1, 0);
    CHECK(b > 0 && (uint64_t)b + L_PAGE <= (uint64_t)a);
    CHECK(!lx_ok((uint64_t)b, 1, RL_VM_W));
    CHECK_EQ(sys(c, NR_mprotect, (uint64_t)a, L_PAGE, L_PROT_READ, 0, 0, 0), 0);
    CHECK(!lx_ok((uint64_t)a, 1, RL_VM_W));
    CHECK(lx_ok((uint64_t)a + L_PAGE, 1, RL_VM_W));
    CHECK_EQ(sys(c, NR_mprotect, (uint64_t)a + 1, L_PAGE, L_PROT_READ, 0, 0, 0), -L_EINVAL);
    CHECK_EQ(sys(c, NR_munmap, (uint64_t)a + L_PAGE, L_PAGE, 0, 0, 0, 0), 0);
    CHECK(!lx_ok((uint64_t)a + L_PAGE, 1, RL_VM_R));
    CHECK_EQ(sys(c, NR_mprotect, (uint64_t)a, 3 * L_PAGE, L_PROT_READ, 0, 0, 0), -L_ENOMEM);
    int64_t f = sys(c, NR_mmap, (uint64_t)a + L_PAGE, L_PAGE, L_PROT_READ | L_PROT_WRITE, L_MAP_PRIVATE | L_MAP_ANONYMOUS | L_MAP_FIXED, (uint64_t)-1, 0);
    CHECK_EQ(f, a + (int64_t)L_PAGE);
    CHECK_EQ(sys(c, NR_munmap, 0x400001, 1, 0, 0, 0, 0), -L_EINVAL);
}

TEST(exit_group_ends_the_process_with_the_low_eight_bits_of_the_status)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(sys(c, NR_exit_group, 0x1ff, 0, 0, 0, 0, 0), INT64_MIN);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, 0xff);
}

TEST(identity_and_information_calls)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(sys(c, NR_getpid, 0, 0, 0, 0, 0, 0), lx.pid);
    CHECK_EQ(sys(c, NR_gettid, 0, 0, 0, 0, 0, 0), lx.pid);
    CHECK_EQ(sys(c, NR_uname, 0x401000, 0, 0, 0, 0, 0), 0);
    l_utsname u;
    memcpy(&u, rl_mem_base() + 0x401000, sizeof u);
    CHECK(strcmp(u.sysname, "Linux") == 0);
    CHECK(strcmp(u.machine, "aarch64") == 0);
    CHECK_EQ(sizeof u, 390);
    CHECK_EQ(sys(c, NR_getrandom, 0x401200, 32, 0, 0, 0, 0), 32);
    CHECK_EQ(sys(c, NR_getrandom, 0x401200, 32, 0x80, 0, 0, 0), -L_EINVAL);
    CHECK_EQ(sys(c, NR_clock_gettime, L_CLOCK_MONOTONIC, 0x401300, 0, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_clock_gettime, 99, 0x401300, 0, 0, 0, 0), -L_EINVAL);
    int64_t ts[2];
    CHECK_EQ(sys(c, NR_clock_gettime, L_CLOCK_REALTIME, 0x401300, 0, 0, 0, 0), 0);
    memcpy(ts, rl_mem_base() + 0x401300, 16);
    CHECK(ts[0] > 1577836800 && ts[1] >= 0 && ts[1] < 1000000000);
    CHECK_EQ(sys(c, NR_ioctl, 1, 0x1234, 0, 0, 0, 0), -L_ENOTTY);
    CHECK_EQ(sys(c, NR_futex, 0x401000, L_FUTEX_WAKE | L_FUTEX_PRIVATE, 1, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_futex, 0x401000, L_FUTEX_WAIT | L_FUTEX_PRIVATE, 12345, 0, 0, 0), -L_EAGAIN);
}

TEST(signal_masks_and_actions_round_trip_and_default_actions_end_the_process)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t set = (1ull << (L_SIGUSR1 - 1)) | (1ull << (L_SIGKILL - 1));
    rl_vm_kernel_write(&lx.vm, 0x401000, &set, 8);
    CHECK_EQ(sys(c, NR_rt_sigprocmask, L_SIG_BLOCK, 0x401000, 0x401008, 8, 0, 0), 0);
    uint64_t old;
    memcpy(&old, rl_mem_base() + 0x401008, 8);
    CHECK_EQ(old, 0);
    CHECK_EQ(lx_cur()->blocked, 1ull << (L_SIGUSR1 - 1));
    CHECK_EQ(sys(c, NR_rt_sigprocmask, L_SIG_BLOCK, 0x401000, 0, 4, 0, 0), -L_EINVAL);
    CHECK_EQ(sys(c, NR_kill, (uint64_t)lx.pid, L_SIGUSR1, 0, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_rt_sigpending, 0x401020, 8, 0, 0, 0, 0), 0);
    uint64_t pend;
    memcpy(&pend, rl_mem_base() + 0x401020, 8);
    CHECK_EQ(pend, 1ull << (L_SIGUSR1 - 1));
    uint64_t act[4] = { L_SIG_IGN, 0, 0, 0 };
    rl_vm_kernel_write(&lx.vm, 0x401040, act, sizeof act);
    CHECK_EQ(sys(c, NR_rt_sigaction, L_SIGUSR1, 0x401040, 0x401080, 8, 0, 0), 0);
    uint64_t prev[4];
    memcpy(prev, rl_mem_base() + 0x401080, sizeof prev);
    CHECK_EQ(prev[0], L_SIG_DFL);
    CHECK_EQ(sys(c, NR_rt_sigaction, L_SIGKILL, 0x401040, 0, 8, 0, 0), -L_EINVAL);
    CHECK_EQ(sys(c, NR_rt_sigaction, 65, 0x401040, 0, 8, 0, 0), -L_EINVAL);
    CHECK_EQ(sys(c, NR_kill, (uint64_t)lx.pid + 1000, L_SIGUSR1, 0, 0, 0, 0), -L_ESRCH);
    uint64_t none = 0;
    rl_vm_kernel_write(&lx.vm, 0x401000, &none, 8);
    CHECK_EQ(sys(c, NR_rt_sigprocmask, L_SIG_SETMASK, 0x401000, 0, 8, 0, 0), 0);
    CHECK_EQ(sys(c, NR_getpid, 0, 0, 0, 0, 0, 0), lx.pid);
    CHECK_EQ(sys(c, NR_rt_sigaction, L_SIGUSR2, 0, 0, 8, 0, 0), 0);
    CHECK_EQ(sys(c, NR_kill, (uint64_t)lx.pid, L_SIGUSR2, 0, 0, 0, 0), INT64_MIN);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, 128 + L_SIGUSR2);
}

TEST(the_kernel_header_values_match_the_uapi_headers_where_the_host_has_them)
{
#if defined(__linux__) && defined(__has_include)
#if __has_include(<asm-generic/errno.h>)
#include <asm-generic/errno.h>
    CHECK_EQ(L_ENOSYS, 38);
    CHECK_EQ(L_ENOTEMPTY, ENOTEMPTY);
    CHECK_EQ(L_ELOOP, ELOOP);
    CHECK_EQ(L_ENOTSUP, 95);
    CHECK_EQ(L_ETIMEDOUT, ETIMEDOUT);
    CHECK_EQ(L_EOVERFLOW, EOVERFLOW);
#endif
#if __has_include(<asm-generic/unistd.h>)
#define __ARCH_WANT_NEW_STAT
#define __SYSCALL(nr, sym)
#define __SC_COMP(nr, sym, comp)
#define __SC_3264(nr, sym32, sym64)
#define __SC_COMP_3264(nr, sym32, sym64, comp)
#include <asm-generic/unistd.h>
    CHECK_EQ(NR_read, __NR_read);
    CHECK_EQ(NR_write, __NR_write);
    CHECK_EQ(NR_openat, __NR_openat);
    CHECK_EQ(NR_mmap, __NR_mmap);
    CHECK_EQ(NR_brk, __NR_brk);
    CHECK_EQ(NR_exit_group, __NR_exit_group);
    CHECK_EQ(NR_rt_sigaction, __NR_rt_sigaction);
    CHECK_EQ(NR_clock_gettime, __NR_clock_gettime);
    CHECK_EQ(NR_getrandom, __NR_getrandom);
    CHECK_EQ(NR_newfstatat, __NR_newfstatat);
    CHECK_EQ(NR_statx, __NR_statx);
    CHECK_EQ(NR_futex, __NR_futex);
    CHECK_EQ(NR_ppoll, __NR_ppoll);
#endif
#endif
}

static void *waiter_main(void *arg)
{
    int64_t *out = arg;
    struct rl_thread *rt = rl_thread_alloc();
    lx_thread *t = calloc(1, sizeof *t);
    t->rt = rt;
    t->waker = hio_waker_new();
    rt->guest = t;
    rl_thread_bind(rt);
    *out = lx_futex_wait(0x401000, 5, -1, ~0u);
    return NULL;
}

TEST(futex_wake_releases_a_waiter_and_a_changed_word_does_not_wait)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint32_t v = 5;
    rl_vm_kernel_write(&lx.vm, 0x401000, &v, 4);
    CHECK_EQ(lx_futex_wait(0x401000, 6, -1, ~0u), -L_EAGAIN);
    CHECK_EQ(lx_futex_wait(0x401000, 5, 1000000, ~0u), -L_ETIMEDOUT);
    pthread_t th;
    int64_t result = 1;
    CHECK_EQ(pthread_create(&th, NULL, waiter_main, &result), 0);
    int woke = 0;
    for (int i = 0; i < 2000 && !woke; i++) {
        woke = lx_futex_wake(0x401000, 1, ~0u);
        rl_sleep_ns(1000000);
    }
    pthread_join(th, NULL);
    CHECK_EQ(woke, 1);
    CHECK_EQ(result, 0);
    CHECK_EQ(lx_futex_wake(0x401000, 1, ~0u), 0);
}

TEST(a_clone_that_is_not_a_thread_is_refused)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(sys(c, NR_clone, 17, 0, 0, 0, 0, 0), -L_ENOSYS);
}

static void *kick_main(void *arg)
{
    lx_thread *t = arg;
    rl_sleep_ns(20000000);
    uint64_t one = 1;
    (void)one;
    t->pending[L_SIGUSR1] = (lx_pending_sig){ .queued = true };
    lx_thread_kick(t);
    return NULL;
}

TEST(a_signal_interrupts_a_blocking_pipe_read_and_a_sleep)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t act[4] = { 0x400000, 0, 0, 0 };
    rl_vm_kernel_write(&lx.vm, 0x401040, act, sizeof act);
    CHECK_EQ(sys(c, NR_rt_sigaction, L_SIGUSR1, 0x401040, 0, 8, 0, 0), 0);
    CHECK_EQ(sys(c, NR_pipe2, 0x401000, 0, 0, 0, 0, 0), 0);
    int32_t fds[2];
    memcpy(fds, rl_mem_base() + 0x401000, 8);
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, kick_main, lx_cur()), 0);
    uint64_t t0 = rl_clock_ns();
    CHECK_EQ(lx_wait(lx_host_fd(fds[0]), L_POLLIN, 5000000000ll), -L_EINTR);
    CHECK(rl_clock_ns() - t0 < 2000000000ull);
    pthread_join(th, NULL);
    lx_cur()->pending[L_SIGUSR1].queued = false;
    CHECK_EQ(lx_wait(-1, 0, 1000000), 0);
    put_str(0x401100, "x");
    CHECK_EQ(sys(c, NR_write, (uint64_t)fds[1], 0x401100, 1, 0, 0, 0), 1);
    CHECK_EQ(lx_wait(lx_host_fd(fds[0]), L_POLLIN, 1000000000ll), L_POLLIN);
}

#ifndef _WIN32
/** @brief Writes a file of the text at the host path. */
static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

TEST(a_guest_root_holds_the_programs_paths_and_the_kernel_views_stay_the_hosts)
{
    char cwd[1024], root[1200], p[1400];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    snprintf(root, sizeof root, "%s/lxroot.XXXXXX", cwd);
    CHECK(mkdtemp(root) != NULL);
    snprintf(p, sizeof p, "%s/usr", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/usr/lib", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/usr/lib/real", root);
    write_text(p, "guest file\n");
    snprintf(p, sizeof p, "%s/usr/lib/abs", root);
    CHECK_EQ(symlink("/usr/lib/real", p), 0);
    snprintf(p, sizeof p, "%s/lib", root);
    CHECK_EQ(symlink("usr/lib", p), 0);
    snprintf(p, sizeof p, "%s/proc", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/usr/lib/gconv", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/usr/lib/gconv/other", root);
    write_text(p, "real\n");
    prog.root = root;
    rl_ctx *c = boot();
    CHECK(c != NULL);
    const uint64_t at = 0x401000, buf = 0x401800;
    put_str(at, "/lib/abs");
    int64_t fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0);
    CHECK(fd >= 3);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fd, buf, 64, 0, 0, 0), 11);
    CHECK(memcmp(cstr(buf), "guest file\n", 11) == 0);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
    put_str(at, "/lib/gconv/other");
    fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0);
    CHECK(fd >= 3);
    CHECK_EQ(sys(c, NR_read, (uint64_t)fd, buf, 64, 0, 0, 0), 5);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
    put_str(at, "/lib/gconv/conf");
    fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0);
    CHECK(fd >= 3);
    CHECK(lx_virt_fd((int)fd) != NULL);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
    lx.dynamic = true;
    put_str(at, "/usr/lib/py/mod.cpython-313-aarch64-linux-gnu.so");
    CHECK_EQ(sys(c, NR_newfstatat, (uint64_t)(int64_t)L_AT_FDCWD, at, buf, 0, 0, 0), -L_ENOENT);
    CHECK_EQ(sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0), -L_ENOENT);
    lx.dynamic = false;
    put_str(at, "/usr/lib/absent");
    CHECK_EQ(sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0), -L_ENOENT);
    put_str(at, "/proc/self/status");
    fd = sys(c, NR_openat, (uint64_t)(int64_t)L_AT_FDCWD, at, L_O_RDONLY, 0, 0, 0);
    CHECK(fd >= 3);
    CHECK_EQ(sys(c, NR_close, (uint64_t)fd, 0, 0, 0, 0, 0), 0);
#endif
    put_str(at, cwd);
    CHECK_EQ(sys(c, NR_newfstatat, (uint64_t)(int64_t)L_AT_FDCWD, at, buf, 0, 0, 0), 0);
    put_str(at, "/lib");
    CHECK_EQ(sys(c, NR_chdir, at, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(sys(c, NR_getcwd, buf, 256, 0, 0, 0, 0), 9);
    CHECK(strcmp(cstr(buf), "/usr/lib") == 0);
#if defined(__linux__)
    put_str(at, "/proc/self/cwd");
    CHECK_EQ(sys(c, NR_readlinkat, (uint64_t)(int64_t)L_AT_FDCWD, at, buf, 256, 0, 0), 8);
    CHECK(memcmp(cstr(buf), "/usr/lib", 8) == 0);
    put_str(at, "/proc/self/exe");
    CHECK_EQ(sys(c, NR_readlinkat, (uint64_t)(int64_t)L_AT_FDCWD, at, buf, 256, 0, 0), 9);
    CHECK(memcmp(cstr(buf), "/bin/prog", 9) == 0);
#endif
    put_str(at, "abs");
    CHECK_EQ(sys(c, NR_readlinkat, (uint64_t)(int64_t)L_AT_FDCWD, at, buf, 256, 0, 0), 13);
    CHECK(memcmp(cstr(buf), "/usr/lib/real", 13) == 0);
    CHECK_EQ(chdir(cwd), 0);
    prog.root = NULL;
    snprintf(p, sizeof p, "%s/usr/lib/abs", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/usr/lib/real", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/usr/lib/gconv/other", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/usr/lib/gconv", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/lib", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/usr/lib", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/usr", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/proc", root);
    rmdir(p);
    rmdir(root);
}
#endif
