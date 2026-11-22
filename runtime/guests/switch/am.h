/**
 * @file am.h
 * @brief Host-facing API of the applet manager: the message queue the guest
 * reads through ICommonStateGetter, focus and operation mode, and the few
 * values other services must agree on.
 */
#ifndef RELIFT_AM_H
#define RELIFT_AM_H

#include "hos.h"

/** @brief Applet messages (nn::am::AppletMessage). */
enum {
    RL_AM_MSG_CHANGE_INTO_FOREGROUND = 1,
    RL_AM_MSG_CHANGE_INTO_BACKGROUND = 2,
    RL_AM_MSG_EXIT = 4,
    RL_AM_MSG_APPLICATION_EXITED = 6,
    RL_AM_MSG_FOCUS_STATE_CHANGED = 15,
    RL_AM_MSG_RESUME = 16,
    RL_AM_MSG_OPERATION_MODE_CHANGED = 30,
    RL_AM_MSG_PERFORMANCE_MODE_CHANGED = 31
};

/** @brief Focus states (nn::am::FocusState). */
enum { RL_AM_FOCUS_IN = 1, RL_AM_FOCUS_OUT = 2 };

/** @brief Language code "en-US" as the 8 byte settings code read little-endian: the default desired language. */
#define RL_AM_LANGUAGE_CODE_EN_US 0x00000053552D6E65ull

/** @brief Queues msg for the guest and signals the message event. */
RL_RUNTIME_API void rl_am_push_message(uint32_t msg);
/** @brief Changes the focus state and queues FocusStateChanged (and ChangeIntoForeground when focused). */
RL_RUNTIME_API void rl_am_set_focus(bool in_focus);
/** @brief Switches docked or handheld; when it changes, queues OperationModeChanged and PerformanceModeChanged and signals the display resolution event. */
RL_RUNTIME_API void rl_am_set_operation_mode(bool docked);
/** @brief True while docked. */
RL_RUNTIME_API bool rl_am_is_docked(void);
/**
 * @brief The user closed the application. As on the console, a program that is inside an exit request handling section
 * (it called ISelfController LockExit, nn::oe::EnterExitRequestHandlingSection) is asked: the exit request message
 * (RL_AM_MSG_EXIT) is queued, the call returns true, and the program is expected to end itself (the caller bounds the
 * wait). Any other program is ended at once with exit code 0 and the call does not return.
 */
RL_RUNTIME_API bool rl_am_request_exit(void);
/**
 * @brief ISelfController LockExit (lock) and UnlockExit: enter and leave the exit request handling section. With an exit
 * already requested, entering it, or leaving it, ends the process at once, as the console does.
 */
RL_RUNTIME_API void rl_am_lock_exit(bool lock);
/** @brief Sets the language code GetDesiredLanguage returns (default RL_AM_LANGUAGE_CODE_EN_US). */
RL_RUNTIME_API void rl_am_set_desired_language(uint64_t code);
/**
 * @brief Sets the display version GetDisplayVersion returns: the program's display version string (the NACP's 16-byte
 * field, at most 15 characters kept), or NULL when the program's control data does not hold one. With none set the
 * command answers 16 zero bytes, as the console does for an empty field, and logs once that the version is unknown.
 */
RL_RUNTIME_API void rl_am_set_display_version(const char *version);
/** @brief Sets the user id (high, low halves) the launch parameter preselects. Default 1, 0. */
RL_RUNTIME_API void rl_am_set_preselected_user(uint64_t high, uint64_t low);

#endif
