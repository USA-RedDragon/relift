/**
 * @file linux.h
 * @brief The Linux guest: what the generated program.c hands the runtime
 * and the entry point it calls. A program is one statically linked aarch64
 * Linux executable; the runtime is the kernel it runs on.
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

/** @brief A program as the build describes it. */
typedef struct rl_linux_program {
    /** @brief The program's name, for reports. */
    const char *name;
    /** @brief The loadable segments, by address. */
    const rl_linux_segment *segments;
    /** @brief Number of segments. */
    uint32_t segment_count;
    /** @brief The loaded bytes of every segment from image_base. */
    const uint8_t *image;
    /** @brief Guest address of image[0]. */
    uint64_t image_base;
    /** @brief Bytes in image. */
    uint64_t image_size;
    /** @brief Guest address of the entry point. */
    uint64_t entry;
    /** @brief Guest address of the program headers, 0 when none are mapped. */
    uint64_t phdr;
    /** @brief Size of one program header. */
    uint32_t phent;
    /** @brief Number of program headers. */
    uint32_t phnum;
    /** @brief The stack size the file asks for; the main stack is the kernel's default either way. */
    uint64_t stack_size;
    /** @brief AT_HWCAP: the features of the CPU the program was lifted for. */
    uint64_t hwcap;
    /** @brief Lifted units to register. */
    const rl_unit *const *units;
    /** @brief Number of units. */
    uint32_t unit_count;
    /** @brief RL_IMAGE_ID_BYTES bytes identifying the program to hot-loaded units. */
    const uint8_t *image_id;
} rl_linux_program;

/**
 * @brief Starts the program: argv[0] is the program's own name, the rest are
 * its arguments, and the environment is the host's. Returns the program's
 * exit status.
 */
int rl_linux_main(const rl_linux_program *p, int argc, char **argv);

#endif
