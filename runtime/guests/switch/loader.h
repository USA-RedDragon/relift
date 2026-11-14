/**
 * @file loader.h
 * @brief The program description relift emits for a Switch title and the
 * entry that loads it: modules, entry point and the NPDM settings.
 */
#ifndef RELIFT_LOADER_H
#define RELIFT_LOADER_H

#include <relift/abi_core.h>

/** @brief One NSO module as relift lifted it. All offsets are from base. */
typedef struct rl_switch_module {
    /** @brief Module name for reports. */
    const char *name;
    /** @brief Guest load address (relift lifted the code at this address). */
    uint64_t base;
    /** @brief Text, rodata and data at their offsets from base, without bss. */
    const uint8_t *image;
    /** @brief Bytes at image. */
    uint64_t image_size;
    /** @brief Text segment offset and size. */
    uint64_t text_off, text_size;
    /** @brief Rodata segment offset and size. */
    uint64_t ro_off, ro_size;
    /** @brief Data segment offset and size. */
    uint64_t data_off, data_size;
    /** @brief Zero-filled bytes right after data. */
    uint64_t bss_size;
} rl_switch_module;

/** @brief A whole program: its modules and the settings its NPDM gives. */
typedef struct rl_switch_program {
    /** @brief Modules in load order. */
    const rl_switch_module *modules;
    /** @brief Number of modules. */
    uint32_t module_count;
    /** @brief Guest address of the first module's entry (its text start). */
    uint64_t entry;
    /** @brief NPDM address space type (0 32-bit, 1 36-bit, 2 32-bit without alias, 3 39-bit). */
    uint32_t address_space_type;
    /** @brief Main thread priority. */
    int32_t main_prio;
    /** @brief Main thread ideal core. */
    int32_t main_core;
    /** @brief Main thread stack size in bytes; 0 keeps the runtime default. */
    uint64_t main_stack_size;
    /** @brief Program id (svcGetInfo 18). */
    uint64_t program_id;
} rl_switch_program;

/**
 * @brief Starts the Switch guest: brings up the process with the program's
 * settings, registers the static units (rl_static_units), maps every module
 * (text and rodata as CODE_STATIC, data and bss as CODE_MUTABLE) and copies
 * its image, registers fsp-srv on the host directory named by RELIFT_SDMC
 * ("./sdmc" when unset, created when missing), attaches the calling thread
 * as the main thread and runs the entry on a main stack.
 *
 * The guest ends the process through svcExitProcess or a fatal error, which
 * flush stdout and stderr and exit the host process with the guest's status.
 * The function returns only when the entry returns or the main thread exits,
 * with 0 or the exit status, and returns 1 with a message on stderr when the
 * program cannot be loaded. argc and argv are not passed to the guest.
 */
RL_RUNTIME_API int rl_switch_main(const rl_switch_program *p, int argc, char **argv);

#endif
