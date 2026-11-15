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

## Switch keyboard defaults

Player 1 binds the keyboard next to the pad, so a player with no pad
connected can play. The other players have no keys. With no gamepad attached, the window host connects
pad 1 with no buttons, and the keys drive it through these bindings:

| Control | Keys | Control | Keys |
| --- | --- | --- | --- |
| Left stick | `w` `a` `s` `d` | D-pad | arrow keys |
| A | `l`, `space` | B | `k`, `escape` |
| X | `i` | Y | `j` |
| L | `q` | R | `e` |
| ZL | `z` | ZR | `c` |
| Plus | `enter` | Minus | `backspace` |
| Left stick press | `left_shift` | Right stick press | `right_shift` |

The right stick has no key. Rebind keys with `bind.p1.<control>`, such as
`bind.p1.a=gamepad:east,key:f`, or give `p2` and up their own keys. A pad and the keyboard work together: a control follows whichever
input is held hardest.

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

## Input script

For runs with nobody at the controls (headless checks, reproducing a bug),
`RELIFT_INPUT_SCRIPT` holds port controls at given guest frames. It is a
debugging tool, not a setting. Steps are comma separated:

```
<frame>[+<frames>]:<port>.<control>[=<value>]
```

A step holds the control at the value (default 1; -1 to 1 for an axis) from
guest frame `frame` for `frames` frames (default 6). While a step holds a
control, its value replaces what the control's bindings read; other controls
keep reading the host. The guest layer counts frames: on Switch, each buffer
the guest queues to vi and each Vulkan present is one frame. The runtime logs each step as it starts.

```sh
RELIFT_INPUT_SCRIPT='3600:p1.a,3900:p1.a,4200+60:p1.lstick_y=1' ./game
```

`rl_ports_set_script` sets a script from code (tests).

`RELIFT_INPUT_SCRIPT=@<path>` follows a file instead: at each guest frame the
runtime adds the steps appended to the file since the last frame, separated
by commas or newlines. A program that watches the game (for example frames
from `RELIFT_VK_CAPTURE`) can steer it by appending steps; a step whose frame
has already passed starts at once. `rl_ports_follow_script` does the same
from code.
