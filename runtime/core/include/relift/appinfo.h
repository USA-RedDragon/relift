/**
 * @file appinfo.h
 * @brief What the game says about itself to the host: its title per language and its icon. The program's
 * generated info file registers both at start-up (rl_app_set_identity); the window backend reads them to title
 * and decorate its windows. A program that registers nothing has no title and no icon here, and a window then
 * keeps the title its opener passed and no icon.
 */
#ifndef RELIFT_APPINFO_H
#define RELIFT_APPINFO_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers the game's identity; every pointer must outlive the run. Titles come in fallback order
 * (langs[i] is a language key such as "AmericanEnglish", or "default"; names[i] its UTF-8 title). The icon is a
 * PNG in base64 (icon_b64 NULL for none).
 */
RL_RUNTIME_API void rl_app_set_identity(const char *const *langs, const char *const *names, size_t count,
                                        const char *default_lang, const char *icon_b64, size_t icon_b64_len);

/**
 * @brief The title for the first of the locale tags (such as "ja", "en_GB.UTF-8", "zh-Hant-TW", or a language key
 * such as "French") that matches a language the game has, else the registered default language's title, else the
 * first title. NULL when no title is registered. Tags that are NULL or empty are skipped.
 */
RL_RUNTIME_API const char *rl_app_title(const char *const *tags, size_t ntags);

/** @brief The registered icon as PNG bytes (decoded once), or NULL with *size 0 when the game has none. */
RL_RUNTIME_API const uint8_t *rl_app_icon_png(size_t *size);

#ifdef __cplusplus
}
#endif

#endif
