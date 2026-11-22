/**
 * @file textentry_int.h
 * @brief Inside text entry: the open entry's state, shared by the model
 * (textentry.c) and the drawing (textentry_draw.c).
 */
#ifndef RELIFT_TEXTENTRY_INT_H
#define RELIFT_TEXTENTRY_INT_H

#include <relift/textentry.h>

#include <pthread.h>

/** @brief The open entry. Fields are read and written under rl_te_lock. */
typedef struct rl_te_state {
    bool open;
    char header[512], sub[512], guide[1024], ok[64], message[1024];
    uint32_t max_chars, min_chars;
    bool can_cancel, secret;
    char text[RL_TEXT_ENTRY_MAX];
    size_t caret;
    uint64_t typed_ns;
} rl_te_state;

/** @brief Guards rl_te. */
extern pthread_mutex_t rl_te_lock;
/** @brief The open entry. */
extern rl_te_state rl_te;

/** @brief Code points in UTF-8 text. */
uint32_t rl_te_chars(const char *s);
/** @brief Bytes of the UTF-8 sequence that starts with byte c. */
size_t rl_te_seq(unsigned char c);

/** @brief Kinds of button a layout places. */
enum { RL_TE_OK = 0, RL_TE_CANCEL = 1, RL_TE_DELETE = 2, RL_TE_BUTTONS = 3 };

/** @brief Most lines the prompt or the sub text wraps onto. */
#define RL_TE_LINES 8

/** @brief Where things are in a view, as rl_te_layout lays them out. */
typedef struct rl_te_place {
    float s;
    size_t header_at[RL_TE_LINES], header_len[RL_TE_LINES], sub_at[RL_TE_LINES], sub_len[RL_TE_LINES];
    uint32_t nheader, nsub;
    bool hint;
    float strip_y;
    float panel[4], field[4];
    float button[RL_TE_BUTTONS][4];
    bool has_button[RL_TE_BUTTONS];
    float picture[4];
    float keyboard_top;
} rl_te_place;

/** @brief Lays the panel out in a width by height view (state lock held). */
void rl_te_layout(uint32_t width, uint32_t height, bool touch, rl_te_place *out);

#endif
