/**
 * @file loader.h
 * @brief The program description relift emits for a Switch title and the
 * entry that loads it: modules, entry point and the NPDM settings.
 */
#ifndef RELIFT_LOADER_H
#define RELIFT_LOADER_H

#include <relift/abi_core.h>
#include <relift/mods.h>

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
    /** @brief Build id as lower-case hex, NULL when unknown. Data mods match cheat files against the main module's. */
    const char *build_id;
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
    /** @brief Lifted units linked into the program (NULL for none, as in a hot-loading dev build). */
    const rl_unit *const *units;
    /** @brief Number of entries in units. */
    uint32_t unit_count;
    /** @brief Bundled code mods (relift/mods.h), or NULL for none. */
    const rl_mods *mods;
    /** @brief The image id (RL_IMAGE_ID_BYTES bytes) of the guest image set, or NULL when the units carry it. */
    const uint8_t *image_id;
} rl_switch_program;

/**
 * @brief A runtime extension the loader consults while it loads a program,
 * such as the Vulkan forwarding layer. Every function may be NULL.
 */
typedef struct rl_switch_hook {
    /** @brief Name for logs. */
    const char *name;
    /**
     * @brief Called once the process is up, before modules are mapped.
     * *code_free is the first free page after the modules in the code
     * region; the hook may claim pages there (rl_hos_reserve_at) and advance
     * it, which keeps its guest addresses next to the lifted code. Returns 0,
     * or -1 to stop loading.
     */
    int (*start)(void *user, uint64_t *code_free);
    /** @brief Guest address an undefined symbol of a module resolves to, 0 when the hook has none. */
    uint64_t (*import)(void *user, const char *symbol);
    /** @brief Host function that replaces a function a module defines, NULL to keep the module's code. */
    rl_fn (*replace)(void *user, const char *symbol);
    /**
     * @brief For a hook the loader adds by itself: called with the name of
     * each dynamic symbol a module defines or imports; a non-zero result
     * adds the hook to this run. NULL means the hook only runs when the
     * program adds it with rl_switch_add_hook.
     */
    int (*wants)(void *user, const char *symbol);
    /** @brief Passed to every function. */
    void *user;
} rl_switch_hook;

/**
 * @brief Registers a hook with a wants function for rl_switch_main to add on
 * its own (see "Automatic hooks"). The Vulkan forwarding code registers its
 * hook from a constructor, so a program that links it needs no call, and a
 * program that does not link it pays nothing. Returns 0, or -1 when the hook
 * has no wants function or the table is full.
 */
RL_RUNTIME_API int rl_switch_register_auto_hook(const rl_switch_hook *h);

/** @brief Most hooks rl_switch_add_hook takes. */
#define RL_SWITCH_HOOKS 8

/** @brief Adds a hook for the next rl_switch_main; returns 0, or -1 when RL_SWITCH_HOOKS are already added. */
RL_RUNTIME_API int rl_switch_add_hook(const rl_switch_hook *h);

/**
 * @brief Starts the Switch guest: brings up the process with the program's
 * settings, registers the static units (p->units), maps every module
 * (text and rodata as CODE_STATIC, data and bss as CODE_MUTABLE) and copies
 * its image, links the hooks' symbols (below), registers fsp-srv on the
 * host directory named by RELIFT_SDMC ("./sdmc" when unset, created when
 * missing) with the data storage from RELIFT_ROMFS and the add-on content
 * from RELIFT_DLC (see hos_fs.h), attaches a thread as the main thread and
 * runs the entry on a main stack. The main thread is the calling thread in a
 * headless run and a second host thread when a window backend is up
 * (rl_window_main; only when a hook runs, so only for a Vulkan program), so the calling thread can run the window event loop.
 *
 * Automatic hooks: before anything starts, the loader reads the dynamic
 * symbols of every module and adds each hook registered with
 * rl_switch_register_auto_hook whose wants function accepts one. The Vulkan
 * forwarding hook (vk/vk.h) accepts vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr, defined or imported. A program without those symbols
 * starts no hook and no window backend, so it reserves nothing for Vulkan. A
 * program may add a hook itself; it is never added twice.
 *
 * Linking: the loader reads each module's dynamic section (through MOD0).
 * A function the module defines whose name a hook replaces gets an
 * RL_FN_OVERRIDE entry at its address, so every call through a GOT or a
 * pointer reaches the host function; a GLOB_DAT, JUMP_SLOT or ABS64
 * relocation against an undefined symbol a hook resolves is written before
 * the entry runs. A guest dynamic linker (rtld) that runs later keeps its
 * own resolutions, which land on replaced definitions.
 *
 * The guest ends the process through svcExitProcess or a fatal error, which
 * flush stdout and stderr and exit the host process with the guest's status.
 * The function returns only when the entry returns or the main thread exits,
 * with 0 or the exit status, and returns 1 with a message on stderr when the
 * program cannot be loaded. argc and argv are not passed to the guest.
 */
RL_RUNTIME_API int rl_switch_main(const rl_switch_program *p, int argc, char **argv);

#endif
