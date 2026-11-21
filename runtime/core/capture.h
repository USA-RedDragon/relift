/**
 * @file capture.h
 * @brief The RenderDoc capture trigger (setting renderdoc.capture), shared by every guest: a capture covers whole guest
 * presents, counted where a guest hands a finished frame to the shared presenter, so no guest code is involved.
 */
#ifndef RELIFT_CAPTURE_H
#define RELIFT_CAPTURE_H

#include <relift/abi_core.h>

/**
 * @brief A guest present starts: when the setting asks for a capture that begins with this present's frame, and
 * RenderDoc is loaded in the process, starts it.
 */
RL_RUNTIME_API void rl_capture_present_begin(void);
/** @brief A guest present is done: counts it, starts a capture that begins with the next frame, and ends the capture whose last frame this was. */
RL_RUNTIME_API void rl_capture_present_end(void);

#endif
