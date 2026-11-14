/**
 * @file settings.h
 * @brief Run-time settings: a registry of typed, validated keys.
 *
 * Settings come from the layers below, lowest first: the declared default,
 * the per-user settings file, a `--relift-settings <file>` JSON file, each
 * `--relift-set key=value` (later wins), and last the key's environment
 * variable when the setting names one. Unknown keys and invalid values warn
 * and are skipped; they never stop a run.
 *
 * The runtime declares its own keys, a guest layer declares the keys of its
 * host services, and a game declares its own from game.toml. The same
 * validation applies to every layer.
 */
#ifndef RELIFT_SETTINGS_H
#define RELIFT_SETTINGS_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The type of a setting. */
typedef enum rl_setting_type {
    /** @brief true or false. */
    RL_SETTING_BOOL = 0,
    /** @brief A 64-bit integer in [min, max], on a multiple of step from min. */
    RL_SETTING_INT = 1,
    /** @brief A finite double in [min, max], on a multiple of step from min. */
    RL_SETTING_FLOAT = 2,
    /** @brief One of a list of names. */
    RL_SETTING_ENUM = 3,
    /** @brief Any text. */
    RL_SETTING_STRING = 4,
    /** @brief "port:control": a port name and a control name of that port. */
    RL_SETTING_BINDING = 5
} rl_setting_type;

/** @brief When a change takes effect. Only RL_APPLY_START exists so far. */
typedef enum rl_setting_apply {
    /** @brief Read once at start. */
    RL_APPLY_START = 0,
    /** @brief Read while running. */
    RL_APPLY_LIVE = 1
} rl_setting_apply;

/** @brief The declaration of one setting. Every pointer must outlive the run. */
typedef struct rl_setting_def {
    /** @brief The key: lowercase letters, digits, '_' and '.'. */
    const char *key;
    /** @brief The value type. */
    rl_setting_type type;
    /** @brief A short name for a launcher to show. */
    const char *label;
    /** @brief A sentence for a launcher to show. */
    const char *description;
    /** @brief When a change takes effect. */
    rl_setting_apply apply;
    /** @brief An environment variable that overrides every other layer, or NULL. */
    const char *env;
    /** @brief Default of a bool (0 or 1), int or enum (an index into choices). */
    int64_t def_int;
    /** @brief Smallest int. */
    int64_t min_int;
    /** @brief Largest int. */
    int64_t max_int;
    /** @brief Int step from min_int, 0 for any. */
    int64_t step_int;
    /** @brief Default float. */
    double def_float;
    /** @brief Smallest float. */
    double min_float;
    /** @brief Largest float. */
    double max_float;
    /** @brief Float step from min_float, 0 for any. */
    double step_float;
    /** @brief Default string or binding, NULL for empty. */
    const char *def_str;
    /** @brief The names of an enum. */
    const char *const *choices;
    /** @brief The number of names in choices. */
    uint32_t nchoices;
} rl_setting_def;

/** @brief The result of setting a key. */
typedef enum rl_setting_status {
    /** @brief The value was valid and is now set. */
    RL_SETTING_OK = 0,
    /** @brief No setting has this key; a warning was logged. */
    RL_SETTING_UNKNOWN = 1,
    /** @brief The value does not fit the type or range; a warning was logged. */
    RL_SETTING_INVALID = 2
} rl_setting_status;

/** @brief Most settings a run can declare. */
#define RL_SETTINGS_MAX 256u

/**
 * @brief Declares settings. A def already declared (the same pointer) is
 * skipped. Returns the number declared, or -1 and logs when a key repeats a
 * different def, the registry is full or a def is malformed (the whole call
 * then declares nothing).
 */
RL_RUNTIME_API int rl_settings_declare(const rl_setting_def *defs, size_t n);

/** @brief Choices of the runtime's `video` setting, as rl_setting_enum returns them. */
enum { RL_VIDEO_AUTO, RL_VIDEO_SDL3, RL_VIDEO_HEADLESS, RL_VIDEO_NONE };

/**
 * @brief Declares the settings of the runtime core: `video` (the window
 * backend, environment RELIFT_VIDEO). Safe to call again.
 */
RL_RUNTIME_API void rl_settings_declare_runtime(void);

/** @brief Sets key from text, as a --relift-set would. Warns on failure. */
RL_RUNTIME_API rl_setting_status rl_settings_set(const char *key, const char *value);

/**
 * @brief Applies a flat JSON object (key to bool, number or string; null
 * skipped). Returns the number of values applied, or -1 and logs when the
 * text is not such an object.
 */
RL_RUNTIME_API int rl_settings_load_json(const char *text, const char *origin);

/** @brief Reads path and applies it as rl_settings_load_json does; -1 when it cannot be read. */
RL_RUNTIME_API int rl_settings_load_file(const char *path);

/**
 * @brief Applies the settings flags of a command line: every
 * `--relift-settings <file>` (or `=file`) in order, then every
 * `--relift-set <key=value>` (or `=key=value`) in order, so a --relift-set
 * beats a file wherever they stand. A flag without a value warns. Returns the
 * number of values applied.
 */
RL_RUNTIME_API int rl_settings_args(int argc, char *const *argv);

/** @brief Most mods a run can list. */
#define RL_MODS_MAX 256u
/**
 * @brief The ordered mod list: the `mods` array of each settings file (user
 * file first, then each --relift-settings in order), then every
 * `--relift-mods <path>` flag in order. Later wins. The runtime only
 * collects the paths.
 */
RL_RUNTIME_API size_t rl_settings_mod_count(void);
/** @brief The path of mod `index`, or NULL past the end. */
RL_RUNTIME_API const char *rl_settings_mod(size_t index);

/** @brief Forgets every value set so far, and every declaration (tests). */
RL_RUNTIME_API void rl_settings_reset(void);

/** @brief The value of a bool; false when the key is not a declared bool. */
RL_RUNTIME_API bool rl_setting_bool(const char *key);
/** @brief The value of an int; 0 when the key is not a declared int. */
RL_RUNTIME_API int64_t rl_setting_int(const char *key);
/** @brief The value of a float; 0 when the key is not a declared float. */
RL_RUNTIME_API double rl_setting_float(const char *key);
/** @brief The index into choices of an enum; 0 when the key is not a declared enum. */
RL_RUNTIME_API uint32_t rl_setting_enum(const char *key);
/**
 * @brief The text of a string; "" when the key is not a declared string. The
 * pointer stays valid until the key is set again.
 */
RL_RUNTIME_API const char *rl_setting_str(const char *key);
/**
 * @brief A key binding: the port and control names, valid like rl_setting_str.
 * Returns false and sets both to "" when the key is not a declared binding or
 * has no value.
 */
RL_RUNTIME_API bool rl_setting_binding(const char *key, const char **port, const char **control);

/**
 * @brief Writes the schema of every declared setting as JSON into buf: an
 * array of objects with key, type, label, description, apply, default and the
 * type's range or choices. Returns the length the text needs (more than size
 * means it was cut), without the terminating NUL.
 */
RL_RUNTIME_API size_t rl_settings_schema_json(char *buf, size_t size);

/** @brief The host systems of rl_settings_dir_for. */
typedef enum rl_dir_os {
    /** @brief XDG data home, else ~/.local/share. */
    RL_DIR_XDG = 0,
    /** @brief ~/Library/Application Support. */
    RL_DIR_MACOS = 1,
    /** @brief %APPDATA%. */
    RL_DIR_WINDOWS = 2
} rl_dir_os;

/**
 * @brief The per-user folder of the app `app` on `os`, from the given values
 * of HOME (USERPROFILE on Windows), XDG_DATA_HOME and APPDATA (NULL or empty
 * when unset). An XDG_DATA_HOME or APPDATA that is not an absolute path is
 * ignored. Returns false when there is no usable base or out is too small.
 */
RL_RUNTIME_API bool rl_settings_dir_for(char *out, size_t size, rl_dir_os os, const char *app, const char *home,
                                        const char *xdg_data, const char *appdata);
/** @brief rl_settings_dir_for on the host, reading the environment. */
RL_RUNTIME_API bool rl_settings_user_dir(char *out, size_t size, const char *app);

/**
 * @brief Names the app for the per-user folder. With a name, a run with no
 * launcher reads <user dir>/settings.json before the flags and keeps its
 * saves under <user dir>/save unless the save folder is set. Without one the
 * runtime touches no per-user folder.
 */
RL_RUNTIME_API void rl_settings_set_app(const char *app);
/** @brief The name from rl_settings_set_app, or NULL. */
RL_RUNTIME_API const char *rl_settings_app(void);
/** @brief Writes the path of the app's per-user settings file; false without an app name or a base. */
RL_RUNTIME_API bool rl_settings_user_file(char *out, size_t size);

#endif
