use crate::cdecl::declarations_c;
use crate::error::Error;
use crate::model::{Control, ControlKind, Origin, Port, PortSet};

/// The controller layout of one Switch player.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SwitchLayout {
    /// A Pro Controller: every button and both sticks.
    Pro,
    /// Two Joy-Con held as one pad; the same controls as the Pro Controller.
    JoyConPair,
    /// A left Joy-Con held sideways: its stick, d-pad as four face buttons, minus, L and ZL.
    JoyConLeft,
    /// A right Joy-Con held sideways: its stick, face buttons, plus, R and ZR.
    JoyConRight,
}

impl SwitchLayout {
    fn kind(self) -> &'static str {
        match self {
            SwitchLayout::Pro => "Switch Pro Controller",
            SwitchLayout::JoyConPair => "Switch Joy-Con pair",
            SwitchLayout::JoyConLeft => "Switch Joy-Con (left)",
            SwitchLayout::JoyConRight => "Switch Joy-Con (right)",
        }
    }
}

type Row = (&'static str, ControlKind, &'static str, &'static str);

const B: ControlKind = ControlKind::Button;
const X: ControlKind = ControlKind::Axis;

const SWITCH_FACE: &[Row] = &[
    ("a", B, "A", "gamepad:east,key:l,key:space"),
    ("b", B, "B", "gamepad:south,key:k,key:escape"),
    ("x", B, "X", "gamepad:north,key:i"),
    ("y", B, "Y", "gamepad:west,key:j"),
];
const SWITCH_DPAD: &[Row] = &[
    ("dpad_up", B, "D-pad up", "gamepad:dpad_up,key:up"),
    (
        "dpad_right",
        B,
        "D-pad right",
        "gamepad:dpad_right,key:right",
    ),
    ("dpad_down", B, "D-pad down", "gamepad:dpad_down,key:down"),
    ("dpad_left", B, "D-pad left", "gamepad:dpad_left,key:left"),
];
const SWITCH_LEFT: &[Row] = &[
    ("l", B, "L", "gamepad:left_shoulder,key:q"),
    ("zl", B, "ZL", "gamepad:left_trigger,key:z"),
    ("minus", B, "Minus", "gamepad:back,key:backspace"),
    (
        "lstick",
        B,
        "Left stick press",
        "gamepad:left_stick,key:left_shift",
    ),
    (
        "lstick_x",
        X,
        "Left stick horizontal",
        "gamepad:left_x,key:d+,key:a-",
    ),
    (
        "lstick_y",
        X,
        "Left stick vertical",
        "gamepad:left_y,key:w+,key:s-",
    ),
];
const SWITCH_RIGHT: &[Row] = &[
    ("r", B, "R", "gamepad:right_shoulder,key:e"),
    ("zr", B, "ZR", "gamepad:right_trigger,key:c"),
    ("plus", B, "Plus", "gamepad:start,key:enter"),
    (
        "rstick",
        B,
        "Right stick press",
        "gamepad:right_stick,key:right_shift",
    ),
    ("rstick_x", X, "Right stick horizontal", "gamepad:right_x"),
    ("rstick_y", X, "Right stick vertical", "gamepad:right_y"),
];

fn controls(groups: &[&[Row]], keyboard: bool) -> Vec<Control> {
    groups
        .iter()
        .flat_map(|g| g.iter())
        .map(|(id, kind, label, default)| {
            let default = default
                .split(',')
                .filter(|b| keyboard || !b.starts_with("key:"))
                .collect::<Vec<_>>()
                .join(",");
            Control::new(id, *kind).labeled(label).bound(&default)
        })
        .collect()
}

/// One Switch player port, `p1` to `p8`.
pub fn switch_player(number: u8, layout: SwitchLayout) -> Port {
    let groups: Vec<&[Row]> = match layout {
        SwitchLayout::Pro | SwitchLayout::JoyConPair => {
            vec![SWITCH_FACE, SWITCH_DPAD, SWITCH_LEFT, SWITCH_RIGHT]
        }
        SwitchLayout::JoyConLeft => vec![SWITCH_DPAD, SWITCH_LEFT],
        SwitchLayout::JoyConRight => vec![SWITCH_FACE, SWITCH_RIGHT],
    };
    Port::new(
        &format!("p{number}"),
        layout.kind(),
        controls(&groups, number == 1),
    )
    .labeled(&format!("Player {number}"))
}

fn touch() -> Port {
    Port::new(
        "touch",
        "Touch screen",
        vec![
            Control::new("press", B)
                .labeled("Touch")
                .bound("mouse:left"),
            Control::new("x", ControlKind::Trigger).labeled("Touch horizontal"),
            Control::new("y", ControlKind::Trigger).labeled("Touch vertical"),
        ],
    )
    .labeled("Touch screen")
}

fn handheld_pad(
    kind: &str,
    face: &[(&str, &str, &str)],
    shoulders: bool,
    extra_face: &[(&str, &str, &str)],
) -> Port {
    let mut rows: Vec<(&str, &str, &str)> = face.to_vec();
    rows.extend_from_slice(extra_face);
    if shoulders {
        rows.push(("l", "L", "gamepad:left_shoulder,key:q"));
        rows.push(("r", "R", "gamepad:right_shoulder,key:w"));
    }
    rows.extend_from_slice(&[
        ("start", "Start", "gamepad:start,key:enter"),
        ("select", "Select", "gamepad:back,key:backspace"),
        ("up", "Up", "gamepad:dpad_up,gamepad:left_y+,key:up"),
        ("down", "Down", "gamepad:dpad_down,gamepad:left_y-,key:down"),
        ("left", "Left", "gamepad:dpad_left,gamepad:left_x-,key:left"),
        (
            "right",
            "Right",
            "gamepad:dpad_right,gamepad:left_x+,key:right",
        ),
    ]);
    Port::new(
        "p1",
        kind,
        rows.iter()
            .map(|(id, label, d)| Control::new(id, B).labeled(label).bound(d))
            .collect(),
    )
    .labeled("Player 1")
}

const AB: &[(&str, &str, &str)] = &[
    ("a", "A", "gamepad:east,key:x"),
    ("b", "B", "gamepad:south,key:z"),
];
const XY: &[(&str, &str, &str)] = &[
    ("x", "X", "gamepad:north,key:s"),
    ("y", "Y", "gamepad:west,key:a"),
];

const N64_PAD: &[Row] = &[
    ("a", B, "A", "gamepad:south,key:x"),
    ("b", B, "B", "gamepad:west,key:z"),
    ("z", B, "Z", "gamepad:left_trigger,key:c"),
    ("start", B, "Start", "gamepad:start,key:enter"),
    ("l", B, "L", "gamepad:left_shoulder,key:q"),
    ("r", B, "R", "gamepad:right_shoulder,key:e"),
    ("up", B, "D-pad up", "gamepad:dpad_up,key:w"),
    ("down", B, "D-pad down", "gamepad:dpad_down,key:s"),
    ("left", B, "D-pad left", "gamepad:dpad_left,key:a"),
    ("right", B, "D-pad right", "gamepad:dpad_right,key:d"),
    ("c_up", B, "C up", "gamepad:right_y+,key:i"),
    ("c_down", B, "C down", "gamepad:right_y-,key:k"),
    ("c_left", B, "C left", "gamepad:right_x-,key:j"),
    ("c_right", B, "C right", "gamepad:right_x+,key:l"),
    (
        "stick_x",
        X,
        "Stick horizontal",
        "gamepad:left_x,key:right+,key:left-",
    ),
    (
        "stick_y",
        X,
        "Stick vertical",
        "gamepad:left_y,key:up+,key:down-",
    ),
];

/// The platform's native ports. Platforms: `switch` (eight players, each a
/// Pro Controller, plus touch), `gb`, `gbc`, `gba` (one pad), `nds` (one
/// pad plus touch) and `n64` (one controller).
pub fn native_ports(platform: &str) -> Result<PortSet, Error> {
    let ports = match platform {
        "switch" => {
            let mut v: Vec<Port> = (1..=8)
                .map(|n| switch_player(n, SwitchLayout::Pro))
                .collect();
            v.push(touch());
            v
        }
        "gb" => vec![handheld_pad("Game Boy pad", AB, false, &[])],
        "gbc" => vec![handheld_pad("Game Boy Color pad", AB, false, &[])],
        "gba" => vec![handheld_pad("Game Boy Advance pad", AB, true, &[])],
        "nds" => vec![handheld_pad("Nintendo DS pad", AB, true, XY), touch()],
        "n64" => vec![
            Port::new("p1", "Nintendo 64 controller", controls(&[N64_PAD], true))
                .labeled("Player 1"),
        ],
        other => {
            return Err(Error::BadDeclaration {
                source: "native ports".to_string(),
                why: format!("`{other}` is not a platform with ports"),
            });
        }
    };
    PortSet::native(ports)
}

/// The id of the on-screen controller's port.
pub const TOUCHPAD_PORT: &str = "touchpad";

const TOUCHPAD_CONTROLS: &[&str] = &[
    "a",
    "b",
    "x",
    "y",
    "l",
    "r",
    "zl",
    "zr",
    "start",
    "select",
    "plus",
    "minus",
    "up",
    "down",
    "left",
    "right",
    "dpad_up",
    "dpad_right",
    "dpad_down",
    "dpad_left",
    "lstick",
    "rstick",
    "lstick_x",
    "lstick_y",
    "rstick_x",
    "rstick_y",
];

/// The on-screen controller's port of a platform with a touch layout (`gb`,
/// `gbc`, `gba`, `switch`): the controls of `p1` it draws, each bound to its
/// own shape (`touch:<control>`). The runtime merges each into the `p1`
/// control of the same id. `None` for a platform without a layout.
pub fn touchpad_port(platform: &str) -> Option<Port> {
    if !matches!(platform, "gb" | "gbc" | "gba" | "switch") {
        return None;
    }
    let set = native_ports(platform).ok()?;
    let p1 = set.port("p1")?;
    let controls = p1
        .controls
        .iter()
        .filter(|c| TOUCHPAD_CONTROLS.contains(&c.id.as_str()))
        .map(|c| {
            let mut t = Control::new(&c.id, c.kind).bound(&format!("touch:{}", c.id));
            t.label.clone_from(&c.label);
            t.origin = Origin::Host;
            t
        })
        .collect();
    let mut port =
        Port::new(TOUCHPAD_PORT, "On-screen controller", controls).labeled("On-screen controller");
    port.origin = Origin::Host;
    Some(port)
}

/// The text of a generated C file declaring a platform's native ports as
/// `<symbol>` and `<symbol>_count`, and for a platform with a touch layout
/// its on-screen controller's port as `<symbol>_touchpad` and
/// `<symbol>_touchpad_count`.
pub fn native_c_file(platform: &str, symbol: &str) -> Result<String, Error> {
    let set = native_ports(platform)?;
    let mut text = format!(
        "/**\n * @file\n * @brief Generated by relift-input (`native_c_file`): the native ports of\n * the `{platform}` platform. Do not edit; `RELIFT_BLESS=1 cargo test -p relift-input`\n * rewrites it.\n */\n#include <relift/ports.h>\n\n{}",
        declarations_c(symbol, set.ports())
    );
    if let Some(t) = touchpad_port(platform) {
        text.push('\n');
        text.push_str(&declarations_c(&format!("{symbol}_touchpad"), &[t]));
    }
    Ok(text)
}
