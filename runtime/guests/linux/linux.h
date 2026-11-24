/**
 * @file linux.h
 * @brief The Linux guest: what the generated program.c hands the runtime
 * and the entry point it calls. A program is an aarch64 Linux executable, with
 * its program interpreter and libraries when it is dynamically linked; the
 * runtime is the kernel it runs on.
 */
#ifndef RL_LINUX_H
#define RL_LINUX_H

#include <relift/arm64/abi.h>
#include <stdint.h>

/** @brief What the pages of a segment are for, as the ELF program header says. */
enum rl_linux_segment_perm {
    /** @brief Read only. */
    RL_LINUX_R = 0,
    /** @brief Read and write. */
    RL_LINUX_RW = 1,
    /** @brief Read and execute. */
    RL_LINUX_RX = 2,
    /** @brief Read, write and execute. */
    RL_LINUX_RWX = 3
};

/** @brief One loadable segment. */
typedef struct rl_linux_segment {
    /** @brief Guest address. */
    uint64_t addr;
    /** @brief Size in memory; the bytes past file_size read as zero. */
    uint64_t mem_size;
    /** @brief Where its bytes start in rl_linux_program.image. */
    uint64_t image_offset;
    /** @brief How many bytes the image holds for it. */
    uint64_t file_size;
    /** @brief enum rl_linux_segment_perm. */
    uint32_t perm;
} rl_linux_segment;

/** @brief What a file of the program is to the process. */
enum rl_linux_role {
    /** @brief The executable the kernel starts. */
    RL_LINUX_MAIN = 0,
    /** @brief The program interpreter: mapped and entered by the kernel. */
    RL_LINUX_INTERPRETER = 1,
    /** @brief A library the interpreter maps from the file system. */
    RL_LINUX_LIBRARY = 2,
    /** @brief A file the program reads that is not code (a plugin directory's configuration): it opens by path and has no segments. */
    RL_LINUX_DATA = 3
};

/**
 * @brief One file of the program: an executable, the program interpreter or
 * a library, placed at the address its code was lifted for.
 */
typedef struct rl_linux_module {
    /** @brief The file's name, for reports. */
    const char *name;
    /** @brief enum rl_linux_role. */
    uint32_t role;
    /** @brief The guest paths the file answers to when it is opened. */
    const char *const *paths;
    /** @brief Number of paths. */
    uint32_t path_count;
    /** @brief The file as it is on disk. */
    const uint8_t *file;
    /** @brief Its size. */
    uint64_t file_size;
    /** @brief The loadable segments, by address; image_offset is a file offset. */
    const rl_linux_segment *segments;
    /** @brief Number of segments. */
    uint32_t segment_count;
    /** @brief The page-aligned guest address the first segment loads at. */
    uint64_t base;
    /** @brief Bytes from base to the end of the last segment, page rounded. */
    uint64_t span;
    /** @brief The alignment of base the loader asks of the file. */
    uint64_t align;
    /** @brief Guest address of the entry point. */
    uint64_t entry;
    /** @brief Guest address of the program headers, 0 when none are mapped. */
    uint64_t phdr;
    /** @brief Size of one program header. */
    uint32_t phent;
    /** @brief Number of program headers. */
    uint32_t phnum;
} rl_linux_module;

/** @brief A program as the build describes it. */
typedef struct rl_linux_program {
    /** @brief The program's name, for reports. */
    const char *name;
    /** @brief The guest path of the executable: what /proc/self/exe and AT_EXECFN name, and what $ORIGIN of the executable is. */
    const char *exec_path;
    /** @brief The files, the executable first. */
    const rl_linux_module *modules;
    /** @brief Number of modules. */
    uint32_t module_count;
    /** @brief The lowest guest address any module loads at. */
    uint64_t code_lo;
    /** @brief One above the highest. */
    uint64_t code_hi;
    /** @brief The stack size the file asks for; the main stack is the kernel's default either way. */
    uint64_t stack_size;
    /** @brief AT_HWCAP: the features of the CPU the program was lifted for. */
    uint64_t hwcap;
    /** @brief AT_HWCAP2: the rest of those features. */
    uint64_t hwcap2;
    /** @brief Lifted units to register. */
    const rl_unit *const *units;
    /** @brief Number of units. */
    uint32_t unit_count;
    /** @brief RL_IMAGE_ID_BYTES bytes identifying the program to hot-loaded units. */
    const uint8_t *image_id;
    /** @brief The class maps of the executable pages (rl_code_map), or NULL for none. */
    const rl_code_map *const *code_maps;
    /** @brief Number of code maps. */
    uint32_t code_map_count;
    /**
     * @brief The host directory that is the guest's root file system (the
     * sysroot of the import), or NULL to use the host's paths as they are:
     * an absolute guest path whose first component the directory holds
     * opens below it, except /proc, /dev, /sys, /tmp and /run.
     */
    const char *root;
} rl_linux_program;

/**
 * @brief Starts the program: argv[0] is the program's own name, the rest are
 * its arguments, and the environment is the host's. Returns the program's
 * exit status.
 */
int rl_linux_main(const rl_linux_program *p, int argc, char **argv);

#endif
