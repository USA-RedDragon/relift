/**
 * @file textentry.h
 * @brief Text entry: a guest asks the player for a line of text (a name, a
 * password), as a console's software keyboard does. In a run with a window
 * and a person at it, the module shows a panel over the game picture and
 * takes typed text from the host; otherwise it answers at once from the
 * settings, so scripted and headless runs never wait.
 *
 * The answer, in order: the setting `text_entry.text` when it is set; else,
 * when no panel can be shown (no host registered, or an input script drives
 * the run), the guest's initial text, or `text_entry.default` when the guest
 * gives none; else the panel's text when the player confirms or cancels.
 *
 * Threads: a guest calls open and close from any thread, a host calls end
 * from any thread. The done callback runs on the thread that ends the entry, with no lock of
 * this module held, and must not call back into it.
 */
#ifndef RELIFT_TEXTENTRY_H
#define RELIFT_TEXTENTRY_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Most bytes of UTF-8 text an entry holds, with its terminator. */
#define RL_TEXT_ENTRY_MAX 2048u

/** @brief What the guest asks for. Strings are UTF-8 and may be NULL; the module copies them. */
typedef struct rl_text_entry_args {
    /** @brief The question, shown in bold at the top. */
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

/** @brief What a host does for the module. */
typedef struct rl_text_entry_host {
    /**
     * @brief Shows the panel for args and returns true, or returns false when it cannot (the module then answers
     * from the settings). The host ends the entry with rl_text_entry_end.
     */
    bool (*show)(const rl_text_entry_args *args);
    /** @brief Takes the panel down without an answer (the guest closed the entry). */
    void (*hide)(void);
} rl_text_entry_host;

/** @brief Registers the host that can show the panel, or NULL for none. */
RL_RUNTIME_API void rl_text_entry_set_host(const rl_text_entry_host *host);
/** @brief Asks for text; see the file comment for where the answer comes from. */
RL_RUNTIME_API rl_text_entry_status rl_text_entry_open(const rl_text_entry_args *args, rl_text_entry_done_fn *done,
                                                      void *user);
/** @brief Ends the open entry of user without calling done; once it returns done will not run for it. */
RL_RUNTIME_API void rl_text_entry_close(void *user);
/** @brief The host's panel ended the open entry: confirmed with text, or canceled. */
RL_RUNTIME_API void rl_text_entry_end(bool confirmed, const char *text);
/** @brief Forgets everything (tests). */
RL_RUNTIME_API void rl_text_entry_reset(void);

#endif
