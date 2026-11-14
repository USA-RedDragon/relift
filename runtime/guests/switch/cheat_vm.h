/**
 * @file cheat_vm.h
 * @brief The cheat virtual machine for Switch cheat codes (the format the
 * Atmosphere dmnt cheat manager documents). It knows nothing of the guest
 * process: memory, region bases and the held keys come through cheat_env.
 * Written from the public format description.
 */
#ifndef RELIFT_CHEAT_VM_H
#define RELIFT_CHEAT_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Most opcode words in one cheat. */
#define CHEAT_MAX_WORDS 0x400u
/** @brief Number of general registers, saved registers and static registers. */
#define CHEAT_REGS 16u
#define CHEAT_STATICS 0x80u

/** @brief Result of a memory access made for a cheat. */
enum cheat_mem {
    /** @brief Done. */
    CHEAT_MEM_OK = 0,
    /** @brief The range is not mapped or not accessible. */
    CHEAT_MEM_UNMAPPED = 1,
    /** @brief A write to lifted code: the cheat needs a rebuild. */
    CHEAT_MEM_CODE = 2
};

/** @brief How a cheat reaches the process. */
typedef struct cheat_env {
    /** @brief Passed to every function. */
    void *user;
    /** @brief Reads n bytes (1, 2, 4 or 8) at a guest address. */
    int (*read)(void *user, uint64_t addr, void *dst, unsigned n);
    /** @brief Writes n bytes at a guest address. */
    int (*write)(void *user, uint64_t addr, const void *src, unsigned n);
    /** @brief Base of memory region 0 main module, 1 heap, 2 alias, 3 aslr; 0 for others. */
    uint64_t (*region_base)(void *user, unsigned region);
    /** @brief Held keys in the dmnt key mask encoding, or NULL for none. */
    uint64_t (*keys)(void *user);
} cheat_env;

/** @brief One cheat's opcodes and the edge state of its key conditions. */
typedef struct cheat_program {
    /** @brief Opcode words. */
    const uint32_t *ops;
    /** @brief Number of words. */
    size_t n;
    /** @brief n bytes of state for key conditions; may be NULL when there are none. */
    uint8_t *edge;
} cheat_program;

/** @brief State that outlives one run: the static registers shared by every cheat. */
typedef struct cheat_vm {
    /** @brief Static registers. */
    uint64_t statics[CHEAT_STATICS];
} cheat_vm;

/** @brief What one run did. */
typedef struct cheat_result {
    /** @brief Memory writes made. */
    uint32_t writes;
    /** @brief Reads and writes that hit unmapped memory. */
    uint32_t faults;
    /** @brief Writes refused because the target is lifted code. */
    uint32_t code_writes;
    /** @brief Address of the first refused code write. */
    uint64_t code_addr;
    /** @brief True when the run stopped at an opcode it cannot decode. */
    bool bad_opcode;
    /** @brief Word index of that opcode. */
    size_t bad_at;
} cheat_result;

/** @brief Runs p once from its start with fresh registers (the statics in vm keep their values). */
void cheat_vm_run(cheat_vm *vm, const cheat_env *env, const cheat_program *p, cheat_result *out);

/** @brief Number of words the instruction at ops[i] takes (n - i at most), 0 when it cannot decode. */
size_t cheat_vm_length(const uint32_t *ops, size_t n, size_t i);

#endif
