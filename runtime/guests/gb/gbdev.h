/**
 * @file gbdev.h
 * @brief The Game Boy devices (picture unit, sound unit, timer, DMA engines,
 * serial port, joypad) behind the scheduler's device slots, from the device
 * library (gbdev_abi.h).
 *
 * This is the exact per-instruction-boundary mode. The library owns the
 * whole bus (cartridge, video RAM, work RAM, OAM and the I/O registers), since
 * the picture unit and the DMA engines read and write those, and it counts
 * dots itself: each access the CPU side issues is one M-cycle, placed where
 * the hardware places it. The slot devices therefore have no catch-up of their
 * own and raise no scheduler lines (interrupt dispatch is part of the
 * instruction boundary, rlgbd_begin); the CPU side keeps the scheduler's clock
 * at rl_gbdev_now with rl_gbdev_sync. Lifted code that charges its own ticks
 * in this mode leaves them to the library.
 */
#ifndef RELIFT_GBDEV_H
#define RELIFT_GBDEV_H

#include "gb.h"
#include "gbdev_abi.h"

/** @brief A machine's devices bound to the memory layer and the scheduler. */
typedef struct rl_gbdev rl_gbdev;

/**
 * @brief Creates the devices for the cartridge of g (it must outlive them),
 * makes them g's bus (rl_gb_set_bus) and adds them to s as g's seven slot
 * devices (rl_gb_attach). g must be fresh from rl_gb_create. Returns NULL when
 * the library refuses the image or s cannot take the devices.
 */
rl_gbdev *rl_gbdev_create(rl_gb *g, rl_sched *s);
/** @brief Frees the devices; g is not usable afterwards. */
void rl_gbdev_destroy(rl_gbdev *d);
/** @brief The library's handle, for the instruction-level calls. */
rlgbd *rl_gbdev_handle(rl_gbdev *d);
/** @brief The position in dots. */
uint64_t rl_gbdev_now(rl_gbdev *d);
/** @brief Moves the clock of c (ratio 1: one cycle per dot) to the library's position. */
void rl_gbdev_sync(rl_gbdev *d, rl_sched_cpu *c);

#endif
