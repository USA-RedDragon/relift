/**
 * @file textentry.h
 * @brief Text entry: a guest asks the player for a line of text (a name, a
 * password), as a console's software keyboard does. In a run with a window
 * and a person at it, a panel over the game picture takes typed text from the
 * host's keyboard or the system's own on-screen keyboard; otherwise the
 * module answers at once from the settings, so scripted and headless runs
 * never wait.
 *
 * The answer, in order: the setting `text_entry.text` when it is set; else,
 * when no panel can be shown (no host registered, the host cannot show it,
 * an input script drives the run and `text_entry.panel` is not `always`, or
 * it is `never`), the guest's initial text, or
 * `text_entry.default` when the guest gives none; else the panel's text when
 * the player confirms or cancels.
 *
 * The module keeps the panel's text and draws the panel (rl_text_entry_draw)
 * with the TrueType fonts the program ships (`fonts/<n>.ttf`, rl_asset_load),
 * so it shows any character a keyboard or input method produces that the
 * fonts cover. The game's picture keeps its full size; one compact line
 * floats over it, near the bottom with a physical keyboard, and right above
 * the system keyboard (which covers the lower part of the view) on touch
 * screens.
 *
 * Threads: a guest calls open and close from any thread; the host calls the
 * input, key, tap and draw functions from its own threads. The done callback
 * runs on the thread that ends the entry with no lock of this module held,
 * and must not call back into it.
 */
#ifndef RELIFT_TEXTENTRY_H
#define RELIFT_TEXTENTRY_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Most bytes of UTF-8 text an entry holds, with its terminator. */
#define RL_TEXT_ENTRY_MAX 2048u

/** @brief What the guest asks for. Strings are UTF-8 and may be NULL; the module copies them. */
typedef struct rl_text_entry_args {
    /** @brief The question, shown at the top. */
    const char *header;
    /** @brief A second line under it. */
    const char *sub;
    /** @brief Shown gray in the empty field. */
    const char *guide;
    /** @brief The text the field starts with. */
    const char *initial;
    /** @brief The word for confirming ("OK" when NULL or empty). */
    const char *ok_label;
    /** @brief A message about the text the guest refused last time, shown under the field. */
    const char *message;
    /** @brief Most characters (code points); 0 for no limit below RL_TEXT_ENTRY_MAX. */
    uint32_t max_chars;
    /** @brief Fewest characters confirming takes. */
    uint32_t min_chars;
    /** @brief True when the player may cancel. */
    bool can_cancel;
    /** @brief True to draw the text as dots. */
    bool secret;
} rl_text_entry_args;

/** @brief The end of an entry: confirmed with text, or canceled (text is then the field's text). */
typedef void rl_text_entry_done_fn(void *user, bool confirmed, const char *text);

/** @brief What open did. */
typedef enum rl_text_entry_status {
    /** @brief Answered at once: done ran before open returned. */
    RL_TEXT_ENTRY_ANSWERED = 0,
    /** @brief The panel is shown; done runs when the player ends it. */
    RL_TEXT_ENTRY_SHOWN = 1,
    /** @brief Another entry is open; nothing happened. */
    RL_TEXT_ENTRY_BUSY = 2
} rl_text_entry_status;

/** @brief Keys a host passes on while the panel is shown. */
enum rl_text_entry_key {
    RL_TEXT_ENTRY_KEY_CONFIRM = 0,
    RL_TEXT_ENTRY_KEY_CANCEL = 1,
    RL_TEXT_ENTRY_KEY_BACKSPACE = 2,
    RL_TEXT_ENTRY_KEY_DELETE = 3,
    RL_TEXT_ENTRY_KEY_LEFT = 4,
    RL_TEXT_ENTRY_KEY_RIGHT = 5,
    RL_TEXT_ENTRY_KEY_HOME = 6,
    RL_TEXT_ENTRY_KEY_END = 7
};

/** @brief What a host does for the module. */
typedef struct rl_text_entry_host {
    /**
     * @brief The panel opens: start taking text (and open the system keyboard on a touch screen) and draw it;
     * return false when the panel cannot be shown (the module then answers from the settings).
     */
    bool (*show)(void);
    /** @brief The panel closed: stop taking text and show the game again. */
    void (*hide)(void);
    /** @brief The panel's look changed (text, caret): draw it again. Any thread. */
    void (*changed)(void);
} rl_text_entry_host;

/** @brief Registers the host that can show the panel, or NULL for none. */
RL_RUNTIME_API void rl_text_entry_set_host(const rl_text_entry_host *host);
/** @brief Asks for text; see the file comment for where the answer comes from. */
RL_RUNTIME_API rl_text_entry_status rl_text_entry_open(const rl_text_entry_args *args, rl_text_entry_done_fn *done,
                                                      void *user);
/** @brief Ends the open entry of user without calling done; once it returns done will not run for it. */
RL_RUNTIME_API void rl_text_entry_close(void *user);
/** @brief True while the panel is shown: the host keeps its input from the guest. */
RL_RUNTIME_API bool rl_text_entry_active(void);
/** @brief True when the module has a font to draw the panel with. */
RL_RUNTIME_API bool rl_text_entry_can_draw(void);
/** @brief Typed text (UTF-8) at the caret; control characters are dropped and the length limit holds. */
RL_RUNTIME_API void rl_text_entry_input(const char *text);
/** @brief A key (enum rl_text_entry_key). Confirm ends the entry when the text is long enough. */
RL_RUNTIME_API void rl_text_entry_key(int key);
/** @brief A tap or click at (x, y) in a view of width by height pixels drawn with layout touch; true when it hit a button. */
RL_RUNTIME_API bool rl_text_entry_tap(float x, float y, uint32_t width, uint32_t height, bool touch);
/** @brief Where the text field is in a view of width by height pixels (x, y, width, height), for an input method's popup. */
RL_RUNTIME_API void rl_text_entry_field(uint32_t width, uint32_t height, bool touch, int32_t out[4]);
/**
 * @brief Draws the panel into rgb (RGB888, width by height) over under (RGBA8, under_w by under_h, the picture the
 * game showed, scaled to fit), with the line placed for a touch screen or a physical keyboard; now_ns times the caret's blink. False when
 * no entry is open or no font can be read.
 */
RL_RUNTIME_API bool rl_text_entry_draw(uint8_t *rgb, uint32_t width, uint32_t height, const uint8_t *under,
                                       uint32_t under_w, uint32_t under_h, bool touch, uint64_t now_ns);
/**
 * @brief Adds a TrueType font (the bytes stay the caller's) ahead of the shipped ones; the first font with a glyph
 * draws it. For tests and hosts that bring their own.
 */
RL_RUNTIME_API void rl_text_entry_add_font(const uint8_t *data, size_t size);
/** @brief Forgets everything but the shipped fonts (tests). */
RL_RUNTIME_API void rl_text_entry_reset(void);

#endif
