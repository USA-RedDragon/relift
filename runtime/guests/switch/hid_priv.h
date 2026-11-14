/**
 * @file hid_priv.h
 * @brief What the hid service files share: the device core that owns the
 * shared memory, the npad configuration and the update thread.
 */
#ifndef RELIFT_HID_PRIV_H
#define RELIFT_HID_PRIV_H

#include "hid.h"
#include "services.h"

/** @brief Hid result codes (module 202). */
#define HID_RESULT_INVALID_NPAD_DEVICE_TYPE RL_MAKE_RESULT(202, 122)
#define HID_RESULT_INVALID_NPAD_ID_TYPE RL_MAKE_RESULT(202, 123)
#define HID_RESULT_INVALID_DEVICE_INDEX RL_MAKE_RESULT(202, 124)
#define HID_RESULT_INVALID_BUFFER_SIZE RL_MAKE_RESULT(202, 131)
/** @brief Irsensor result codes (module 205). */
#define IRS_RESULT_INVALID_CAMERA_HANDLE RL_MAKE_RESULT(205, 204)
#define IRS_RESULT_INVALID_BUFFER_SIZE RL_MAKE_RESULT(205, 207)
#define IRS_RESULT_NPAD_ID_OUT_OF_RANGE RL_MAKE_RESULT(205, 709)

/** @brief Npad ids as a guest sends them. */
enum {
    HID_ID_PLAYER1 = 0,
    HID_ID_PLAYER8 = 7,
    HID_ID_UNKNOWN = 16,
    HID_ID_HANDHELD = 32
};

/** @brief A slot value that is no npad. */
#define HID_SLOT_NONE 0xFFFFu

/** @brief Npad slot (0 to 9) of a guest npad id; false when the id is not valid. */
bool hid_slot_from_id(uint32_t id, uint32_t *slot);
/** @brief Npad id of a slot. */
uint32_t hid_id_from_slot(uint32_t slot);

/** @brief Returns the shared memory (creating it and starting the update thread on first use), with a new reference for the caller; NULL when out of memory. */
hos_shmem *hid_core_open(void);
/** @brief Registers hid:sys, hid:dbg, irs and irs:sys. */
void hid_sys_install(void);
/** @brief Drops all state; called by the install function at rl_hos_init. */
void hid_core_reset(void);

/** @brief Devices that a guest activates. */
enum hid_device {
    HID_DEV_DEBUG_PAD,
    HID_DEV_TOUCH,
    HID_DEV_MOUSE,
    HID_DEV_DEBUG_MOUSE,
    HID_DEV_KEYBOARD,
    HID_DEV_NPAD
};

/** @brief Activates a device and fills its ring with 17 current entries. */
void hid_core_activate(enum hid_device d);

/** @brief Supported style set. */
void hid_core_set_styles(uint32_t styles);
uint32_t hid_core_styles(void);
/** @brief Replaces the supported npad ids. */
void hid_core_set_players(const uint32_t *ids, uint32_t n);
/** @brief True when the slot is among the supported ids. */
bool hid_core_player_supported(uint32_t slot);
/** @brief Joy hold type. */
void hid_core_set_joy_hold(uint64_t hold);
uint64_t hid_core_joy_hold(void);
/** @brief Joy assignment mode of a slot (0 dual, 1 single). */
void hid_core_set_assignment(uint32_t slot, uint32_t mode);
/** @brief Style update event of a slot (borrowed, not referenced); NULL when out of memory. */
hos_revent *hid_core_style_event(uint32_t slot);
/** @brief Applet footer ui type of a slot (0 without shared memory). */
uint8_t hid_core_footer_type(uint32_t slot);
/** @brief Last vibration values per slot and position, in/out (4 floats). */
void hid_core_set_vibration(uint32_t slot, uint32_t position, const float v[4]);
void hid_core_get_vibration(uint32_t slot, uint32_t position, float v[4]);

#endif
