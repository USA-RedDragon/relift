/**
 * @file ios.h
 * @brief What an iOS program needs besides the runtime's layers: its entry under UIKit and its Documents folder.
 * Implemented by host/ios_sdl3.c; only on iOS.
 */
#ifndef RELIFT_IOS_H
#define RELIFT_IOS_H

#include <relift/abi_core.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

/**
 * @brief The program's entry. The glue's main hands it to SDL, which calls it once UIKit has launched the app;
 * the app exits with its status when it returns.
 */
int rl_ios_main(int argc, char **argv);

/** @brief Line-buffers stdout and stderr and makes the app's Documents the working directory. Call first in rl_ios_main. */
RL_RUNTIME_API void rl_ios_init(void);

/** @brief Flushes what the program printed. Call before rl_ios_main returns. */
RL_RUNTIME_API void rl_ios_flush(void);

/** @brief The app bundle's folder (no trailing slash), or NULL when SDL cannot tell. */
RL_RUNTIME_API const char *rl_ios_bundle_dir(void);

/** @brief Logs the AVAudioSession category and the ports of its current output route. */
RL_RUNTIME_API void rl_ios_log_audio_route(void);

/** @brief The app's Documents folder (no trailing slash; the Files app shows it), or NULL when SDL cannot tell. */
RL_RUNTIME_API const char *rl_ios_documents_dir(void);

#endif

#endif
