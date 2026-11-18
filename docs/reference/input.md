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
| NDS | `p1` and `touch` |

Android builds of Switch, GB, GBC and GBA games also have the port
`touchpad`, the on-screen controller (below).

The tables live in `core/input/src/native.rs`. The C tables the runtime reads
are generated from them (`runtime/guests/switch/ports_gen.c`,
`runtime/guests/gb/ports_gen.c`, `runtime/guests/nds/run/ports_gen.c`); a test fails when they are stale, and
`RELIFT_BLESS=1 cargo test -p relift-input` rewrites them. Joy-Con layouts
(pair, left and right sideways) are subsets of the Pro Controller controls.
Motion is not declared yet.

## Nintendo DS

`p1` holds the DS's buttons with the GBA pad's bindings plus X (`gamepad:north`,
`key:s`) and Y (`gamepad:west`, `key:a`). The program reads the ports once per
frame. `touch.press` (`mouse:left`) puts the pen down: with the button held over
the window, at the bottom screen's pixel under the pointer, in either screen
layout (`video.nds_layout`); a press over the top screen or the margins touches
nothing. Without a held mouse button, the pen goes to `touch.x` and `touch.y`
(0 to 1 across the bottom screen), which an input script can hold, for example
`RELIFT_INPUT_SCRIPT=120:touch.press,120:touch.x=0.5,120:touch.y=0.5`. The lid
stays open.

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

## On-screen controller

On a touch screen (Android) the runtime draws a controller over the game
picture: the port `touchpad`, listed in the self-description with
`origin = "host"`. Its controls are the `p1` controls it draws, each bound by
default to its own shape (`bind.touchpad.a = "touch:a"`), so it can be
rebound like any port. Each frame its values merge into `p1`: a `p1` control
reads pressed when its own bindings or the `touchpad` control of the same id
say so (axes take the larger magnitude). Builds for other hosts show it only
with `input.touch_pad.show = always`; the port is declared only when it can
show.

| Setting | Values | Default |
| --- | --- | --- |
| `input.touch_pad.show` | `auto` (only while no gamepad is connected; keyboard and mouse do not count), `always`, `never` | `auto` |
| `input.touch_pad.opacity` | 20 to 100 (percent) | 60 |
| `input.touch_pad.size` | 70 to 150 (percent) | 100 |
| `input.touch_pad.haptics` | `true`, `false`: a short tick on a press or a d-pad direction change | `true` |

When a gamepad connects the controller releases what it holds, fades out in
150 ms and a toast says so; it fades back in when the last one disconnects.
Up to 10 fingers: a finger belongs to the first shape it lands on until it
lifts. The d-pad has eight zones and follows a sliding finger; a finger slides
between face buttons; sticks are fixed with a 10% dead zone; hit areas are
about 15% larger than drawn and at least 48 dp. On Switch the landscape
picture keeps its full size with the controller over its corners, and a
finger no shape takes is the `touch` port's.

The Hide button hides every other control and stays faintly visible as Show;
the hidden state is not saved. The Menu tab and the system back gesture open
the pause sheet: Resume, Edit controller layout, Settings (the settings above)
and Leave game. While a sheet or the editor is open the game is paused.

The editor moves and resizes each group and changes the spacing between a
group's buttons (0.60 to 1.80 times the distance between centers). Holding a
button detaches it to move and resize it alone; Attach puts it back. D-pads
and stick bases are not detachable. Reset returns the guest and orientation to
the built-in layout. Places are saved, per guest and orientation, as:

```
input.touch_pad.layout.<guest>.<orientation>.<group> = "x,y,scale,spacing"
input.touch_pad.layout.<guest>.<orientation>.control.<control> = "x,y,scale"
```

with x and y the center as a fraction of the screen. The runtime writes
setting changes back into the settings file a launcher passed with
`--relift-settings`, or the per-user `settings.json`, keeping the other
entries.

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
