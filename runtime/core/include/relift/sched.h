/**
 * @file relift/sched.h
 * @brief The device scheduler, for guests whose programs drive devices
 * through memory-mapped registers and depend on CPU timing relative to them
 * (docs/abi.md section 13).
 *
 * One rl_sched runs one guest machine: a timeline in ticks, a clock per guest
 * CPU, the devices with the address ranges they answer, an event queue for
 * timed device work and the interrupt lines.
 *
 * - Clock: each CPU counts its own cycles in an rl_time (abi_core.h), at
 *   cycle deadline - budget. One cycle is `ratio` ticks of the timeline, so
 *   CPUs with different rates share it.
 * - Budget: lifted code subtracts each block's cost from budget and calls
 *   rl_sync when it is zero or below. rl_sync runs the events that are due,
 *   sets a budget that ends at the next event and enters the interrupt the
 *   guest takes, if any (the CPU clock's rl_irq_hook decides).
 * - Catch-up: devices are lazy. A device is advanced (rl_device.catch_up)
 *   only when something may observe it: an access to a range that names it,
 *   an event of its own, or rl_sched_sync. Each range names the devices that
 *   may observe or affect an access to it, and an access first brings each of
 *   them, in registration order, to the access's tick.
 * - Several CPUs: one host thread runs every CPU of a scheduler, each on
 *   its own fiber (a host stack of its own, rl_sched_spawn), and one runs at
 *   a time. When a CPU would get more than `quantum` ticks ahead of another
 *   active CPU, or reaches an event the others have not reached, it hands the
 *   run to the CPU furthest behind: a switch of host stacks that takes a few
 *   nanoseconds (docs/abi.md section 14). Events fire once every active CPU
 *   has reached them. The order depends on the clocks alone, so a run is
 *   repeatable. With one CPU every access and event is exact to the tick.
 * - A CPU that was not spawned runs on the stack of the host thread that
 *   calls rl_sched_run for it; at most one CPU of a scheduler does, and not
 *   while rl_sched_main runs. One CPU alone needs no fiber.
 *
 * Only the host thread that runs the scheduler calls these functions; device
 * and event callbacks run on it, on the fiber of the CPU that holds the run.
 */
#ifndef RELIFT_SCHED_H
#define RELIFT_SCHED_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Most CPUs one scheduler drives. */
#define RL_SCHED_MAX_CPUS 8u
/** @brief Most devices one scheduler holds: a device set is a 64-bit mask of rl_device_bit values. */
#define RL_SCHED_MAX_DEVICES 64u
/** @brief Number of interrupt lines, 0 to 63: a set of lines is a 64-bit mask. */
#define RL_SCHED_LINES 64u
/** @brief rl_device.line of a device without an interrupt line. */
#define RL_LINE_NONE UINT32_MAX
/** @brief Default longest budget in ticks when no event is nearer (rl_sched_config.slice). */
#define RL_SCHED_SLICE_DEFAULT (1ull << 20)
/** @brief Default host stack of a spawned CPU in bytes (rl_sched_config.stack), reserved, with a guard below. */
#define RL_SCHED_STACK_DEFAULT (16ull << 20)

/** @brief A scheduler; opaque. */
typedef struct rl_sched rl_sched;
/** @brief The scheduler's record of one guest CPU; opaque. */
typedef struct rl_sched_cpu rl_sched_cpu;
/** @brief A device. */
typedef struct rl_device rl_device;
/** @brief A timed event. */
typedef struct rl_event rl_event;

/**
 * @brief Advances d from tick `from` (its rl_device.now) to tick `to`: the
 * work of every tick in between (counters, line output, raised lines).
 * Called only with to > from.
 */
typedef void rl_device_catch_up(rl_device *d, uint64_t from, uint64_t to);
/** @brief Reads size bytes (1, 2, 4 or 8) at guest address addr at tick now; d is already caught up to now. */
typedef uint64_t rl_device_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now);
/** @brief Writes size bytes of value at guest address addr at tick now; d is already caught up to now. */
typedef void rl_device_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now);

/** @brief A device: filled by the guest layer, registered with rl_sched_add_device, owned by the caller. */
struct rl_device {
    /** @brief Name for reports. */
    const char *name;
    /** @brief Advances the device, or NULL for a device whose state changes only on access. */
    rl_device_catch_up *catch_up;
    /** @brief Read handler for the ranges it owns, or NULL (reads return all ones). */
    rl_device_read *read;
    /** @brief Write handler for the ranges it owns, or NULL (writes are dropped). */
    rl_device_write *write;
    /** @brief The guest layer's state for the device. */
    void *opaque;
    /** @brief The device's interrupt line (rl_device_raise), or RL_LINE_NONE. */
    uint32_t line;
    /** @brief Set by rl_sched_add_device: the device's bit in device sets is 1 << index. */
    uint32_t index;
    /** @brief Set by the scheduler: the tick the device has been advanced to. */
    uint64_t now;
    /** @brief Set by rl_sched_add_device. */
    rl_sched *sched;
};

/** @brief Called when an event is due, with the tick it was posted for. It may post events, raise lines and access devices. */
typedef void rl_event_fire(rl_sched *s, rl_event *e, uint64_t when);

/** @brief A timed event: the caller fills fire, dev and opaque, queues it with rl_sched_post and owns it. */
struct rl_event {
    /** @brief The callback. */
    rl_event_fire *fire;
    /** @brief A device the scheduler catches up to the event's tick before fire, or NULL. */
    rl_device *dev;
    /** @brief The caller's state. */
    void *opaque;
    /** @brief Set by rl_sched_post: the tick it fires at. */
    uint64_t when;
    /** @brief Set by rl_sched_post: orders events of one tick by post order. */
    uint64_t seq;
    /** @brief Queue position plus one, 0 when not queued. */
    uint32_t slot;
};

/**
 * @brief The interrupt hook of a CPU layer or guest layer, called by
 * rl_sync. Returns the guest pc of the interrupt entry after doing the
 * guest-visible entry (saving next, masking, acknowledging, charging the
 * entry cycles to c's rl_time), or RL_NO_IRQ.
 */
typedef uint64_t rl_irq_hook(rl_ctx *c, uint8_t *mem, uint64_t next);

/** @brief Scheduler settings for rl_sched_create. */
typedef struct rl_sched_config {
    /** @brief Longest budget in ticks when no event is nearer; 0 means RL_SCHED_SLICE_DEFAULT. */
    uint64_t slice;
    /** @brief Most ticks a CPU may run ahead of another active CPU; 0 means 1. */
    uint64_t quantum;
    /** @brief Host stack bytes of each spawned CPU; 0 means RL_SCHED_STACK_DEFAULT. */
    uint64_t stack;
} rl_sched_config;

/** @brief Counters for tests and benchmarks. */
typedef struct rl_sched_stats {
    /** @brief rl_sched_run calls. */
    uint64_t runs;
    /** @brief Events fired. */
    uint64_t events;
    /** @brief Device catch_up calls. */
    uint64_t catch_ups;
    /** @brief Device reads and writes. */
    uint64_t accesses;
    /** @brief Accesses no range covered. */
    uint64_t unmapped;
    /** @brief Hand-overs of the run between CPUs. */
    uint64_t switches;
    /** @brief Interrupts entered by rl_sync. */
    uint64_t irqs;
} rl_sched_stats;

/** @brief Creates a scheduler at tick 0, or NULL (out of memory). cfg may be NULL for the defaults. */
RL_RUNTIME_API rl_sched *rl_sched_create(const rl_sched_config *cfg);
/** @brief Frees the scheduler, its CPU records and their fibers; devices and events stay the caller's. Not called from a CPU's fiber. */
RL_RUNTIME_API void rl_sched_destroy(rl_sched *s);
/** @brief The counters. */
RL_RUNTIME_API const rl_sched_stats *rl_sched_get_stats(const rl_sched *s);

/**
 * @brief Adds a CPU at cycle 0 with ratio ticks per cycle; it is active, and
 * the first one added holds the run. Its clock is the scheduler's own until
 * rl_sched_attach. Returns NULL when RL_SCHED_MAX_CPUS is reached, ratio is
 * 0 or the scheduler has run.
 */
RL_RUNTIME_API rl_sched_cpu *rl_sched_add_cpu(rl_sched *s, uint32_t ratio);
/**
 * @brief Binds c to a guest thread: c's clock becomes the rl_time in t's
 * context (rl_cpu_desc.time_offset), with its current value, and rl_sync and
 * rl_io_read on that context reach c. Returns 0, or -1 when the CPU layer has
 * no clock.
 */
RL_RUNTIME_API int rl_sched_attach(rl_sched_cpu *c, struct rl_thread *t);
/** @brief The rl_time c counts in. */
RL_RUNTIME_API rl_time *rl_sched_clock(rl_sched_cpu *c);
/** @brief Sets the hook rl_sync asks about interrupts, or NULL for none. */
RL_RUNTIME_API void rl_sched_set_irq(rl_sched_cpu *c, rl_irq_hook *hook);
/** @brief The scheduler of c. */
RL_RUNTIME_API rl_sched *rl_sched_of(const rl_sched_cpu *c);
/** @brief The CPU that holds the run (the one whose access or sync is being served), or NULL. */
RL_RUNTIME_API rl_sched_cpu *rl_sched_current(rl_sched *s);
/** @brief The code a spawned CPU runs: body(c, arg) on c's fiber. */
typedef void rl_sched_body(rl_sched_cpu *c, void *arg);
/**
 * @brief Gives c its own fiber, which runs body(c, arg) once c first holds
 * the run (body calls rl_sched_run before its first block, as the budget
 * starts at 0). When body returns, c leaves. Call on the host thread that
 * runs the scheduler, which also gets an alternate signal stack here if it
 * has none. Returns 0, or -1 when body is NULL, c has a fiber, the scheduler
 * has run or the host refuses the stack.
 */
RL_RUNTIME_API int rl_sched_spawn(rl_sched_cpu *c, rl_sched_body *body, void *arg);
/**
 * @brief Runs the CPUs on the calling host thread until no CPU holds the
 * run, that is until every CPU has left. Returns 0, or -1 when called from a
 * CPU's fiber or when an active CPU was not spawned.
 */
RL_RUNTIME_API int rl_sched_main(rl_sched *s);
/**
 * @brief Makes c inactive (its clock stops bounding the others) and hands the
 * run on when c holds it. On c's own fiber it does not return. On a CPU that
 * was not spawned it returns once no CPU holds the run.
 */
RL_RUNTIME_API void rl_sched_leave(rl_sched_cpu *c);
/** @brief Changes c's ticks per cycle from its current cycle on and ends its budget. */
RL_RUNTIME_API void rl_sched_set_ratio(rl_sched_cpu *c, uint32_t ratio);

/** @brief The cycle c is at. */
RL_RUNTIME_API uint64_t rl_sched_cycles(const rl_sched_cpu *c);
/** @brief The tick c is at. */
RL_RUNTIME_API uint64_t rl_sched_now(const rl_sched_cpu *c);
/** @brief The tick up to which events have run: every active CPU has reached it. */
RL_RUNTIME_API uint64_t rl_sched_time(const rl_sched *s);

/**
 * @brief The budget's slow path without interrupt entry: runs due events,
 * may hand the run to another CPU (a switch to its fiber) until the run comes
 * back, then sets a new budget (always above zero). Returns nonzero when a raised line of c is in
 * c's mask.
 */
RL_RUNTIME_API int rl_sched_run(rl_sched_cpu *c);
/** @brief Ends c's budget now without moving its clock (deadline = now, budget = 0). */
RL_RUNTIME_API void rl_sched_break(rl_sched_cpu *c);
/** @brief Moves c's clock forward by cycles (a stall or wait state charged by a device), keeping its deadline. */
RL_RUNTIME_API void rl_sched_stall(rl_sched_cpu *c, uint64_t cycles);
/**
 * @brief Lets c wait for an interrupt: moves its clock to each next deadline
 * and runs the scheduler until a line in its mask is raised. Returns 1 then,
 * or 0 when nothing could raise one (no event queued and no other running
 * CPU).
 */
RL_RUNTIME_API int rl_sched_halt(rl_sched_cpu *c);

/** @brief Adds a device; sets its index, now (the current time) and sched. Returns 0, or -1 when RL_SCHED_MAX_DEVICES is reached. */
RL_RUNTIME_API int rl_sched_add_device(rl_sched *s, rl_device *d);
/** @brief The device's bit in a device set. */
static inline uint64_t rl_device_bit(const rl_device *d) { return 1ull << d->index; }
/**
 * @brief Maps guest range [addr, addr + size) to owner. An access to it
 * first catches up owner and every device of the set observers. Returns 0, or
 * -1 when the range is empty, wraps, overlaps a mapped one or owner is not
 * added.
 */
RL_RUNTIME_API int rl_sched_map(rl_sched *s, uint64_t addr, uint64_t size, rl_device *owner, uint64_t observers);

/**
 * @brief A read of size bytes at addr by c at its current tick: runs the
 * events due by then, catches up the range's devices and calls the owner's
 * read. An unmapped address reads all ones.
 */
RL_RUNTIME_API uint64_t rl_sched_read(rl_sched_cpu *c, uint64_t addr, uint32_t size);
/** @brief The write twin of rl_sched_read. An unmapped write is dropped. */
RL_RUNTIME_API void rl_sched_write(rl_sched_cpu *c, uint64_t addr, uint32_t size, uint64_t value);
/** @brief Catches up every device of the set to tick when (devices already there are left alone). */
RL_RUNTIME_API void rl_sched_sync(rl_sched *s, uint64_t devices, uint64_t when);

/**
 * @brief Queues e to fire at tick when, or moves it there when queued. A tick
 * already passed fires at the next scheduler run or access. The running CPU's
 * budget is cut so that it stops at when.
 */
RL_RUNTIME_API void rl_sched_post(rl_sched *s, rl_event *e, uint64_t when);
/** @brief Removes e from the queue when queued. */
RL_RUNTIME_API void rl_sched_cancel(rl_sched *s, rl_event *e);
/** @brief True when e is queued. */
static inline bool rl_event_queued(const rl_event *e) { return e->slot != 0; }

/** @brief Routes line to the CPU with index cpu, in the order added (every line starts on the first CPU). */
RL_RUNTIME_API void rl_sched_route(rl_sched *s, uint32_t line, uint32_t cpu);
/** @brief Raises line; when it is in its CPU's mask that CPU's budget ends. */
RL_RUNTIME_API void rl_sched_raise(rl_sched *s, uint32_t line);
/** @brief Lowers line. */
RL_RUNTIME_API void rl_sched_lower(rl_sched *s, uint32_t line);
/** @brief Raises d's line, when it has one. */
RL_RUNTIME_API void rl_device_raise(rl_device *d);
/** @brief Sets the lines that interrupt c; ends its budget when one of them is already raised. */
RL_RUNTIME_API void rl_sched_set_mask(rl_sched_cpu *c, uint64_t mask);
/** @brief The raised lines of c that are in its mask. */
RL_RUNTIME_API uint64_t rl_sched_pending(const rl_sched_cpu *c);

#endif
