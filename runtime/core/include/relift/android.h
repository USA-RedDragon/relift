/**
 * @file android.h
 * @brief What an Android program needs besides the runtime's layers: its stdio in logcat and its private files
 * directory. Implemented by host/android_sdl3.c; only on Android.
 */
#ifndef RELIFT_ANDROID_H
#define RELIFT_ANDROID_H

#include <relift/abi_core.h>

#ifdef __ANDROID__

/**
 * @brief Sends stdout and stderr to logcat (tag "relift", one entry per line) and points HOME at the app's private
 * files directory, so settings and saves land in app storage. Call once, first thing in SDL_main.
 */
RL_RUNTIME_API void rl_android_init(void);

/** @brief Waits until everything the program printed has reached logcat. Call before SDL_main returns: the app dies with it. */
RL_RUNTIME_API void rl_android_flush(void);

/** @brief The app's private files directory (no trailing slash), or NULL when SDL cannot tell. */
RL_RUNTIME_API const char *rl_android_files_dir(void);

/** @brief The app's external files directory (no trailing slash; the one a user can copy data into), or NULL. */
RL_RUNTIME_API const char *rl_android_external_dir(void);

#endif

#endif
