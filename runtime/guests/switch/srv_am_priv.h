/**
 * @file srv_am_priv.h
 * @brief Internals shared by the files of the applet manager group.
 */
#ifndef RELIFT_SRV_AM_PRIV_H
#define RELIFT_SRV_AM_PRIV_H

#include "am.h"
#include "services.h"
#include "srv_link.h"

/** @brief Applet manager results (module 128). */
#define AM_RESULT(desc) RL_MAKE_RESULT(128, desc)
#define AM_NOT_AVAILABLE AM_RESULT(2)
#define AM_NO_MESSAGES AM_RESULT(3)
#define AM_LIBRARY_APPLET_TERMINATED AM_RESULT(22)
#define AM_OBJECT_INVALID AM_RESULT(500)
#define AM_OUT_OF_BOUNDS AM_RESULT(503)
#define AM_BUFFER_NOT_ACQUIRED AM_RESULT(504)
#define AM_BUFFER_ALREADY_ACQUIRED AM_RESULT(505)
#define AM_INVALID_PARAMETERS AM_RESULT(506)
#define AM_INVALID_STORAGE_TYPE AM_RESULT(511)
#define AM_UNBALANCED_FATAL_SECTION AM_RESULT(512)
#define AM_NOT_IMPLEMENTED AM_RESULT(998)
#define AM_STUBBED AM_RESULT(999)

/** @brief Reads command input of type T into var or returns the framework's header error. */
#define AM_IN(T, var)                                                                                                  \
    T var;                                                                                                             \
    if (!rl_srv_in(r, &var, sizeof var))                                                                               \
    return SF_RESULT_INVALID_HEADER

/** @brief Command table entry count. */
#define AM_N(t) ((uint32_t)(sizeof(t) / sizeof((t)[0])))

/** @brief Pops the oldest queued message; false when empty. Updates the message event. */
bool am_msg_pop(uint32_t *msg);
/** @brief The message event (owned by the state). */
hos_revent *am_message_event(void);
/** @brief The display resolution change event (owned by the state). */
hos_revent *am_resolution_event(void);
/** @brief Current focus state (RL_AM_FOCUS_*). */
uint32_t am_focus_state(void);
/** @brief Applet resource user id of process pid, assigned on first use. */
uint64_t am_resource_user_id(uint64_t pid);
/** @brief Language code returned by GetDesiredLanguage. */
uint64_t am_language_code(void);
/** @brief Preselected user id halves. */
void am_preselected_user(uint64_t *high, uint64_t *low);
/** @brief Resets the state of the application side (called by the install function). */
void am_app_reset(void);

/** @brief Returns a copy handle for e, or an error when e is NULL. */
rl_result am_push_event(rl_ipc_req *r, hos_revent *e);
/** @brief Returns object o to the client, or an error when o is NULL. */
rl_result am_push_object(rl_ipc_req *r, rl_ipc_object *o);

rl_ipc_object *am_new_common_state_getter(void);
rl_ipc_object *am_new_self_controller(uint64_t pid);
rl_ipc_object *am_new_window_controller(uint64_t pid);
rl_ipc_object *am_new_audio_controller(void);
rl_ipc_object *am_new_display_controller(void);
rl_ipc_object *am_new_library_applet_creator(void);
rl_ipc_object *am_new_application_functions(void);
rl_ipc_object *am_new_home_menu_functions(void);
rl_ipc_object *am_new_library_self_accessor(void);
rl_ipc_object *am_new_process_winding_controller(void);
rl_ipc_object *am_new_empty(const rl_ipc_class *cls);
/** @brief Switch 2: IPerformanceFunctions. */
rl_ipc_object *am_new_performance_functions(void);
/** @brief Switch 2: IGraphicsScalingParametersFunctions. */
rl_ipc_object *am_new_graphics_scaling_functions(void);
/** @brief Resets the Switch 2 interfaces' state. */
void am_s2_reset(void);

/** @brief A storage holding a copy of size bytes of data (zeros when data is NULL). */
rl_ipc_object *am_storage_new(const void *data, uint64_t size);
/** @brief True when o is an IStorage. */
bool am_storage_is(const rl_ipc_object *o);
/** @brief The size of storage o, 0 when o is not one. */
uint64_t am_storage_size(const rl_ipc_object *o);
/** @brief Reads size bytes at offset of storage o; false when out of bounds or o holds only a handle. */
bool am_storage_read(const rl_ipc_object *o, uint64_t offset, void *dst, uint64_t size);
/** @brief Writes size bytes at offset of storage o; false when out of bounds, read-only or a handle. */
bool am_storage_write(rl_ipc_object *o, uint64_t offset, const void *src, uint64_t size);
/** @brief ILibraryAppletCreator 11 CreateTransferMemoryStorage. */
rl_result am_create_transfer_memory_storage(rl_ipc_req *r);
/** @brief ILibraryAppletCreator 12 CreateHandleStorage. */
rl_result am_create_handle_storage(rl_ipc_req *r);

/** @brief Library applet ids (the low byte of the id an application asks for). */
#define AM_APPLET_ID_SWKBD 0x11u

/** @brief A library applet an application created: its channels, state and implementation. */
typedef struct am_applet am_applet;

/**
 * @brief A library applet relift implements. Its functions run with no
 * applet lock held, on the thread of the request that caused them; the
 * applet answers through the am_applet_* functions below, from any thread.
 */
typedef struct am_applet_impl {
    /** @brief Name for logs. */
    const char *name;
    /** @brief ILibraryAppletAccessor Start: the in channel holds what the application pushed. */
    void (*start)(am_applet *a);
    /** @brief The application pushed interactive in data. May be NULL. */
    void (*interactive)(am_applet *a);
    /**
     * @brief The application asked the applet to end (RequestExit, Terminate) or dropped its accessor. After it
     * returns the implementation calls no am_applet_* function again. May be NULL.
     */
    void (*stop)(am_applet *a);
    /** @brief Frees the implementation's state; the applet is going. May be NULL. */
    void (*destroy)(am_applet *a);
} am_applet_impl;

/** @brief The implementation's state. */
void *am_applet_state(const am_applet *a);
/** @brief Sets the implementation's state. */
void am_applet_set_state(am_applet *a, void *state);
/** @brief The library applet mode the application asked for. */
uint32_t am_applet_mode(const am_applet *a);
/** @brief Takes the oldest storage of the in channel (the caller owns the reference), or NULL. */
rl_ipc_object *am_applet_pop_in(am_applet *a);
/** @brief Takes the oldest storage of the interactive in channel, or NULL. */
rl_ipc_object *am_applet_pop_interactive_in(am_applet *a);
/** @brief Pushes a storage of size bytes to the out channel and signals its event. */
bool am_applet_push_out(am_applet *a, const void *data, uint64_t size);
/** @brief Pushes a storage of size bytes to the interactive out channel and signals its event. */
bool am_applet_push_interactive_out(am_applet *a, const void *data, uint64_t size);
/** @brief The applet ends with result (GetResult); signals the state changed event. Later calls do nothing. */
void am_applet_exit(am_applet *a, rl_result result);

extern const rl_ipc_class am_debug_functions_class;
extern const rl_ipc_class am_global_state_controller_class;
extern const rl_ipc_class am_application_creator_class;
extern const rl_ipc_class am_applet_common_functions_class;

#endif
