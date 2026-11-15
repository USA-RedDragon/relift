/**
 * @file ports.h
 * @brief Input ports: named ports with named controls, bindings from host
 * inputs to those controls, and reading a control's value. A guest layer
 * declares its native ports; a game or mod declares extra ports or controls
 * on top. The module knows no controller layout.
 *
 * Binding text is a comma separated list of host inputs: `gamepad:south`,
 * `gamepad2:left_x-`, `key:z`, `mouse:left`. The names come from input_names.h.
 * A bare `gamepad` means the pad of the control's port: port `pN` reads pad
 * N - 1 and every other port reads pad 0. A `+` or `-` suffix keeps the
 * positive or negative half of a range; a pressed button reads -1 with `-`.
 * `touch:<name>` is a control of the on-screen controller.
 *
 * Each control of port `p1` also reads the control of the same id on port
 * `touchpad` (RL_TOUCHPAD_PORT) when that port is declared: buttons are
 * pressed when either is, axes take the larger magnitude.
 */
#ifndef RELIFT_PORTS_H
#define RELIFT_PORTS_H

#include <relift/abi_core.h>
#include <relift/input.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Control kinds. A button reads 0 or 1, a trigger 0 to 1, an axis -1 to 1. */
enum rl_control_kind {
    RL_CONTROL_BUTTON = 0,
    RL_CONTROL_TRIGGER = 1,
    RL_CONTROL_AXIS = 2
};

/** @brief One declared control. */
typedef struct rl_control_decl {
    /** @brief The control id. */
    const char *id;
    /** @brief enum rl_control_kind. */
    uint8_t kind;
    /** @brief Default binding text, or NULL for none. */
    const char *defaults;
} rl_control_decl;

/** @brief One declared port. */
typedef struct rl_port_decl {
    /** @brief The port id. */
    const char *id;
    /** @brief The controller kind. */
    const char *kind;
    /** @brief Number of entries in controls. */
    uint32_t count;
    /** @brief The controls. */
    const rl_control_decl *controls;
} rl_port_decl;

/** @brief A port and control, found by id; RL_PORT_NONE when absent. */
typedef uint32_t rl_control_ref;
/** @brief The reference of a control that does not exist. It reads 0. */
#define RL_PORT_NONE UINT32_C(0xffffffff)

/**
 * @brief Declares ports. A port whose id is already declared gains the new
 * controls (its kind must match). Returns false and declares nothing for a
 * control id declared twice on a port, a bad default or a kind mismatch.
 */
RL_RUNTIME_API bool rl_ports_declare(const rl_port_decl *ports, uint32_t count);
/** @brief Forgets every port and binding. */
RL_RUNTIME_API void rl_ports_reset(void);
/** @brief Number of declared ports. */
RL_RUNTIME_API uint32_t rl_ports_count(void);
/** @brief Id of port index (declaration order), or NULL past the end. */
RL_RUNTIME_API const char *rl_ports_id(uint32_t index);
/** @brief Number of controls of a port, 0 when it does not exist. */
RL_RUNTIME_API uint32_t rl_ports_control_count(const char *port);
/** @brief Finds a control. */
RL_RUNTIME_API rl_control_ref rl_ports_find(const char *port, const char *control);
/** @brief Replaces a control's bindings from text; false when it does not parse or the control is absent. */
RL_RUNTIME_API bool rl_ports_bind(const char *port, const char *control, const char *text);
/** @brief Restores the default bindings of every control. */
RL_RUNTIME_API void rl_ports_bind_defaults(void);
/**
 * @brief Applies a `bind.<port>.<control>` setting key; returns false when
 * the key is not a binding key or the value does not apply.
 */
RL_RUNTIME_API bool rl_ports_apply_setting(const char *key, const char *value);
/** @brief True when text is a valid binding list (empty is valid). */
RL_RUNTIME_API bool rl_ports_valid_binding(const char *text);
/** @brief Reads a binding list now: the strongest of its inputs. `pad` is the gamepad a bare `gamepad` means. */
RL_RUNTIME_API float rl_ports_read_binding(const char *text, int pad);
/** @brief The value of a control for a snapshot. */
RL_RUNTIME_API float rl_ports_eval(rl_control_ref ref, const rl_input_snapshot *s);
/** @brief The value of a control now, from rl_input_sample. */
RL_RUNTIME_API float rl_ports_read(rl_control_ref ref);

/**
 * @brief Sets the input script, a debug tool: comma separated steps
 * `<frame>[+<frames>]:<port>.<control>[=<value>]`, each holding a control at
 * the value (default 1) for the frames (default 6) from a guest frame on.
 * Without a call, the first rl_ports_script_frame reads RELIFT_INPUT_SCRIPT (`@<path>` follows a file). Returns false and keeps the old script when the text is invalid.
 */
RL_RUNTIME_API bool rl_ports_set_script(const char *text);
/**
 * @brief Follows a script file: each rl_ports_script_frame adds the steps
 * appended to it since, a step whose frame has passed starting at once.
 * Returns false when the file does not open.
 */
RL_RUNTIME_API bool rl_ports_follow_script(const char *path);
/** @brief True when text is a valid input script (empty is valid). */
RL_RUNTIME_API bool rl_ports_valid_script(const char *text);
/** @brief The guest layer tells the input script the number of the frame the guest just finished. */
RL_RUNTIME_API void rl_ports_script_frame(uint64_t frame);
/** @brief True with the value when the input script holds port.control at the current frame. */
RL_RUNTIME_API bool rl_ports_script_value(const char *port, const char *control, float *value);
/** @brief Drops the input script and the frame count (tests). */
RL_RUNTIME_API void rl_ports_script_reset(void);

#endif
