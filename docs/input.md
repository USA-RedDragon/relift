# Input ports and bindings

A relift binary lists its input ports in the `input` section of its
self-description. Each port has an id, a controller kind and controls; each
control has an id, a kind (`button`, `trigger` or `axis`), an optional label
and default bindings.

## Native ports

| Platform | Ports |
| --- | --- |
| Switch | `p1` to `p8` (Pro Controller layout) and `touch` |
| GB, GBC, GBA | `p1` |
| NDS | `p1` and `touch` (declared) |

The tables live in `core/input/src/native.rs`. The C tables the runtime reads
are generated from them (`runtime/guests/switch/ports_gen.c`,
`runtime/guests/gb/ports_gen.c`); a test fails when they are stale, and
`RELIFT_BLESS=1 cargo test -p relift-input` rewrites them. Joy-Con layouts
(pair, left and right sideways) are subsets of the Pro Controller controls.
Motion is not declared yet.

## Extra ports and controls

`game.toml` or a mod adds ports or controls:

```toml
[input.port.dial]
kind = "Rotary dial"
label = "Dial"

[input.port.dial.control.turn]
kind = "axis"
default = "mouse:left"

[input.port.p1.control.turbo]
label = "Turbo"
default = "key:t"
```

A table for a new port needs a `kind`. A table for an existing port can only
add controls. Extra ports and controls are marked in the self-description
(`origin` is `game` or `mod:<name>`, and the port has `extra = true`).

## Bindings

A binding is a comma separated list of host inputs: `gamepad:south`,
`gamepad2:left_x-`, `key:z`, `mouse:left`. Names come from
`runtime/core/include/relift/input_names.h`. A bare `gamepad` is the pad of
the control's port (`pN` reads pad N, any other port reads pad 1; `gamepadN`
names a pad). A `+` or `-` suffix keeps one half of a range.

The key binding setting type is `bind.<port id>.<control id>`; its value is
the list. A launcher passes it like any setting; the runtime applies it with
`rl_ports_apply_setting`.

## Reading input

Overrides include `relift/override.h` and call `relift_input(port, control)`,
which returns a float (button 0 or 1, trigger 0 to 1, axis -1 to 1). For a
control read every frame, keep the result of `rl_ports_find` and call
`rl_ports_read`. The Switch hid service reads the native ports, so a
rebinding changes what the guest sees.
