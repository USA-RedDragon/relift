use crate::cdecl::declarations_c;
use crate::error::Error;
use crate::model::{Control, ControlKind, Port, PortSet};

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
    ("a", B, "A", "gamepad:east"),
    ("b", B, "B", "gamepad:south"),
    ("x", B, "X", "gamepad:north"),
    ("y", B, "Y", "gamepad:west"),
];
const SWITCH_DPAD: &[Row] = &[
    ("dpad_up", B, "D-pad up", "gamepad:dpad_up"),
    ("dpad_right", B, "D-pad right", "gamepad:dpad_right"),
    ("dpad_down", B, "D-pad down", "gamepad:dpad_down"),
    ("dpad_left", B, "D-pad left", "gamepad:dpad_left"),
];
const SWITCH_LEFT: &[Row] = &[
    ("l", B, "L", "gamepad:left_shoulder"),
    ("zl", B, "ZL", "gamepad:left_trigger"),
    ("minus", B, "Minus", "gamepad:back"),
    ("lstick", B, "Left stick press", "gamepad:left_stick"),
    ("lstick_x", X, "Left stick horizontal", "gamepad:left_x"),
    ("lstick_y", X, "Left stick vertical", "gamepad:left_y"),
];
const SWITCH_RIGHT: &[Row] = &[
    ("r", B, "R", "gamepad:right_shoulder"),
    ("zr", B, "ZR", "gamepad:right_trigger"),
    ("plus", B, "Plus", "gamepad:start"),
    ("rstick", B, "Right stick press", "gamepad:right_stick"),
    ("rstick_x", X, "Right stick horizontal", "gamepad:right_x"),
    ("rstick_y", X, "Right stick vertical", "gamepad:right_y"),
];

fn controls(groups: &[&[Row]]) -> Vec<Control> {
    groups
        .iter()
        .flat_map(|g| g.iter())
        .map(|(id, kind, label, default)| Control::new(id, *kind).labeled(label).bound(default))
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
    Port::new(&format!("p{number}"), layout.kind(), controls(&groups))
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
        ("up", "Up", "gamepad:dpad_up,key:up"),
        ("down", "Down", "gamepad:dpad_down,key:down"),
        ("left", "Left", "gamepad:dpad_left,key:left"),
        ("right", "Right", "gamepad:dpad_right,key:right"),
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

/// The platform's native ports. Platforms: `switch` (eight players, each a
/// Pro Controller, plus touch), `gb`, `gbc`, `gba` (one pad) and `nds` (one
/// pad plus touch).
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
        other => {
            return Err(Error::BadDeclaration {
                source: "native ports".to_string(),
                why: format!("`{other}` is not a platform with ports"),
            });
        }
    };
    PortSet::native(ports)
}

/// The text of a generated C file declaring a platform's native ports as
/// `<symbol>` and `<symbol>_count`.
pub fn native_c_file(platform: &str, symbol: &str) -> Result<String, Error> {
    let set = native_ports(platform)?;
    Ok(format!(
        "/**\n * @file\n * @brief Generated by relift-input (`native_c_file`): the native ports of\n * the `{platform}` platform. Do not edit; `RELIFT_BLESS=1 cargo test -p relift-input`\n * rewrites it.\n */\n#include <relift/ports.h>\n\n{}",
        declarations_c(symbol, set.ports())
    ))
}
