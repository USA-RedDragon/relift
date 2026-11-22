//! The game's own settings: the `[settings.<key>]` tables of `relift.toml`,
//! the C that declares them to the runtime, and the build-time check that
//! every `relift_setting_*` read in an override names a declared key of the
//! matching type (design 006).
//!
//! ```toml
//! [settings.fast_text]
//! type = "bool"          # bool, int, float, enum, string, key_binding
//! label = "Fast text"
//! description = "Skips the text scroll."
//! default = false
//!
//! [settings.scale]
//! type = "int"
//! default = 2
//! min = 1
//! max = 4
//! step = 1
//! ```
//!
//! A key binding's value is a comma separated list of host inputs such as
//! `"gamepad:south,key:z"` (see docs/reference/input.md). The keys `bind.<port>.<control>`
//! belong to the runtime, which declares one for every port control.

use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_info::{Apply, Setting, SettingKind};
use serde_json::Value;

use crate::import::BoxError;

/// The reads an override can make, with the setting type each needs.
const READS: [(&str, &str); 7] = [
    ("relift_setting_bool", "bool"),
    ("relift_setting_int", "int"),
    ("relift_setting_float", "float"),
    ("relift_setting_enum", "enum"),
    ("relift_setting_str", "string"),
    ("relift_setting_binding", "key_binding"),
    ("relift_setting_binding_value", "key_binding"),
];

/// Whether `key` is a valid setting key: lowercase letters, digits, `_` and
/// `.`, starting with a letter.
#[must_use]
pub fn key_ok(key: &str) -> bool {
    key.chars().next().is_some_and(|c| c.is_ascii_lowercase())
        && key
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '.')
}

/// The name of a setting's type.
#[must_use]
pub fn type_name(kind: &SettingKind) -> &'static str {
    match kind {
        SettingKind::Bool => "bool",
        SettingKind::Int { .. } => "int",
        SettingKind::Float { .. } => "float",
        SettingKind::Enum { .. } => "enum",
        SettingKind::String => "string",
        SettingKind::KeyBinding => "key_binding",
    }
}

/// The settings the runtime defines, as the self-description lists them.
/// A test keeps this list equal to the declarations in `runtime/`.
#[must_use]
pub fn runtime_settings() -> Vec<Setting> {
    let s = |key: &str, label: &str, description: &str, default: Value, kind| Setting {
        key: key.into(),
        label: label.into(),
        description: description.into(),
        apply: Apply::Start,
        default,
        kind,
    };
    vec![
        s(
            "video",
            "Window backend",
            "auto opens SDL3 when it is built in and a display exists; sdl3 skips the display check; headless (or none) opens no window",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "sdl3", "headless", "none"]
                    .map(String::from)
                    .into(),
            },
        ),
        s(
            "language",
            "Title language",
            "The language of the game's window title: a language name such as AmericanEnglish or French, or a locale such as ja or fr_CA. Empty follows the host's language",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "video.gpu_driver",
            "GPU driver",
            "Android: system uses the device's Vulkan driver; the name of an installed driver package (a meta.json and its library under the app's gpu_drivers folder) loads that driver instead",
            Value::String("system".into()),
            SettingKind::String,
        ),
        Setting {
            apply: Apply::Live,
            ..s(
                "picture.aa",
                "Anti-aliasing",
                "Smooths the edges of the picture the window shows: fxaa is FXAA 3.11, smaa is SMAA 1x. Anything but off changes the picture from the console's",
                Value::String("off".into()),
                SettingKind::Enum {
                    choices: ["off", "fxaa", "smaa"].map(String::from).into(),
                },
            )
        },
        Setting {
            apply: Apply::Live,
            ..s(
                "picture.aa_quality",
                "Anti-aliasing quality",
                "SMAA's preset: higher finds longer and diagonal edges and costs more GPU time. With smaa on it changes the picture from the console's",
                Value::String("high".into()),
                SettingKind::Enum {
                    choices: ["low", "medium", "high", "ultra"].map(String::from).into(),
                },
            )
        },
        Setting {
            apply: Apply::Live,
            ..s(
                "picture.filter",
                "Scaling filter",
                "How windows scale and sample the picture. default: the window's own scaling (whole multiples, or filling the window where a setting such as video.nds_integer_scale asks), nearest at whole multiples, sharp_bilinear above 2x when the scale is not whole, bilinear below 2x. integer: whole multiples only, sharp. nearest: fill the window, sharp uneven pixels. sharp_bilinear: fill the window with even pixels whose edges blend over one screen pixel. Anything but nearest sampling changes the picture from the console's",
                Value::String("default".into()),
                SettingKind::Enum {
                    choices: ["default", "integer", "nearest", "sharp_bilinear"]
                        .map(String::from)
                        .into(),
                },
            )
        },
        Setting {
            apply: Apply::Live,
            ..s(
                "picture.upscaler",
                "Upscaler",
                "The upscaler a game's own upscaler call (such as DLSS) runs inside the frame, before the game's UI: game is the game's own, translated; auto is the game's own where this device meets its requirements, else picked by GPU: taa on integrated and mobile GPUs, and on discrete ones dlss where it runs, else fsr2; off copies the picture at its render size; nearest, bilinear, sharp_bilinear and lanczos are relift's; fsr1 is AMD FSR 1 (EASU and RCAS); fsr2 and fsr3 are AMD FSR 2 and the FSR 3 upscaler, temporal, where the game gives depth and motion vectors, else FSR 1; taa is relift's plain temporal upscaler, else bilinear; dlss is NVIDIA DLSS, temporal, on an NVIDIA GPU with the driver's NGX library and NVIDIA's DLSS library beside the program, else fsr2. Every choice but game changes the picture from the console's",
                Value::String("auto".into()),
                SettingKind::Enum {
                    choices: [
                        "auto",
                        "game",
                        "off",
                        "nearest",
                        "bilinear",
                        "sharp_bilinear",
                        "lanczos",
                        "fsr1",
                        "fsr2",
                        "fsr3",
                        "taa",
                        "dlss",
                    ]
                    .map(String::from)
                    .into(),
                },
            )
        },
        Setting {
            apply: Apply::Live,
            ..s(
                "picture.upscale_scale",
                "Window upscale factor",
                "For guests that do not upscale inside their frame: with picture.upscaler set to one of relift's upscalers, the window's picture is upscaled to this many times its size ahead of anti-aliasing and the window's own scaling",
                Value::from(2),
                SettingKind::Int {
                    min: 1,
                    max: 8,
                    step: 0,
                },
            )
        },
        Setting {
            apply: Apply::Live,
            ..s(
                "picture.taa_sharpness",
                "TAA sharpening",
                "How strongly AMD's RCAS sharpens the picture the taa upscaler writes, from 0 (no sharpening pass) to 1 (its strongest). Anything but 0 changes the picture further from the console's",
                Value::from(0.5),
                SettingKind::Float {
                    min: 0.0,
                    max: 1.0,
                    step: 0.05,
                },
            )
        },
        s(
            "renderdoc.capture",
            "RenderDoc capture",
            "<present>[:<frames>]: when the program runs under RenderDoc, capture that many frames (default 1) from the numbered guest present (counting from 1), including runs with no window. Only Vulkan work is captured. Empty: off",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "renderdoc.capture_dir",
            "RenderDoc capture folder",
            "Where RenderDoc writes the captures renderdoc.capture asks for. Empty: the captures folder of the user data folder",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "text_entry.text",
            "Typed text",
            "When set, the answer to every text the game asks for (a name, for one), given without showing the text entry panel. Empty: a run with a window and no input script shows the panel",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "text_entry.default",
            "Default typed text",
            "The answer when the game asks for text, no panel can be shown (no window, or an input script drives the run) and the game offers no text of its own",
            Value::String("Player".into()),
            SettingKind::String,
        ),
        s(
            "text_entry.panel",
            "Text entry panel",
            "When the panel that takes typed text is shown: auto in a run with a window and no input script; always also when an input script drives the run (the run waits for the player); never answers from the settings at once",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "always", "never"].map(String::from).into(),
            },
        ),
        s(
            "save.dir",
            "Save folder",
            "Where the game's save data is kept. Empty: <user folder>/save when the program names an app, else ./save",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "vk.device",
            "Graphics device",
            "A part of the name of the host graphics device the game uses. Empty: the first device that can present",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "stats.every",
            "Frame stats",
            "Every this many guest frames, log the frames per second, the process CPU and the CPU the presenting thread spent per frame over them. 0: off",
            Value::from(0),
            SettingKind::Int {
                min: 0,
                max: 1_000_000,
                step: 0,
            },
        ),
        s(
            "vi.follow_display",
            "Follow the display's refresh",
            "Keep the game's 60 Hz vsync in step with the display, where the host reports when it shows a frame, so no frame is shown twice or dropped when the display runs slightly off 60 Hz",
            Value::Bool(true),
            SettingKind::Bool,
        ),
        s(
            "stats.trace",
            "Frame trace",
            "A file to write a trace of vsyncs, presents, acquires, scheduler hand-overs and slow svcs to, for finding why a vsync went without a new frame. Empty: off",
            Value::String(String::new()),
            SettingKind::String,
        ),
        s(
            "vk.keep_attachments",
            "Keep attachment contents",
            "Pass the game's DONT_CARE attachment load and store ops as LOAD and STORE, as the console keeps the contents. auto: only on host GPUs known to lose them (Apple)",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "on", "off"].map(String::from).into(),
            },
        ),
        s(
            "vk.decode_bc",
            "Decode compressed textures",
            "Give the host GPU BC1 to BC7 textures as uncompressed ones, decoded on the CPU at upload. auto: only the formats the host GPU cannot sample (Apple, some Adreno and Mali)",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "on", "off"].map(String::from).into(),
            },
        ),
        s(
            "nvn.memory",
            "Graphics memory pools",
            "How the game's graphics memory pools reach the host graphics device. import: the device uses the game's pages in place (needs VK_EXT_external_memory_host); mirror: each pool gets a device copy that the layer keeps equal at the game's sync points, for devices without the import (some Android drivers, MoltenVK) and for GPU capture tools that hide it. auto: import where the device has it",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "import", "mirror"].map(String::from).into(),
            },
        ),
        s(
            "kernel.layout",
            "Address space layout",
            "compact: the regions the game uses packed at the lowest bases, so less host address space is reserved; console: the console's exact region sizes",
            Value::String("compact".into()),
            SettingKind::Enum {
                choices: ["compact", "console"].map(String::from).into(),
            },
        ),
        s(
            "clock.fixed_step",
            "Fixed-step clock (testing)",
            "For tests: run one guest thread at a time on a guest clock that only the game's progress moves, so two runs with the same input script show the same frames. Slower; vi.follow_display is off",
            Value::Bool(false),
            SettingKind::Bool,
        ),
        s(
            "cpu.pac",
            "Pointer authentication cipher",
            "fast is a keyed hash; qarma5 is the console's QARMA5. A game sees the same behavior with either, since it cannot learn the keys",
            Value::String("fast".into()),
            SettingKind::Enum {
                choices: ["fast", "qarma5"].map(String::from).into(),
            },
        ),
        s(
            "cpu.pac_seed",
            "Pointer authentication key seed",
            "The seed of the process's pointer authentication keys, which the console picks at random",
            Value::from(0),
            SettingKind::Int {
                min: 0,
                max: i64::MAX,
                step: 0,
            },
        ),
    ]
}

/// The setting of the Game Boy layer (`runtime/guests/gb/exact/settings.c`), which
/// a test keeps equal to its declaration.
#[must_use]
pub fn gb_settings() -> Vec<Setting> {
    vec![Setting {
        key: "video.gb_color_correction".into(),
        label: "Colour correction".into(),
        description: "Shows the colours as the LCD of the console does. Off: the plain expansion of the 5-bit channels the game writes".into(),
        apply: Apply::Start,
        default: Value::Bool(true),
        kind: SettingKind::Bool,
    }]
}

/// The screen settings of the Nintendo DS layer (`runtime/guests/nds/settings.c`),
/// which a test keeps equal to their declaration.
#[must_use]
pub fn nds_settings() -> Vec<Setting> {
    let s = |key: &str, label: &str, description: &str, default: Value, kind| Setting {
        key: key.into(),
        label: label.into(),
        description: description.into(),
        apply: Apply::Live,
        default,
        kind,
    };
    vec![
        s(
            "video.nds_layout",
            "Screen layout",
            "How the two screens share a window. auto: stacked in a tall window, else whichever of stacked, side by side and large top shows the most picture, with the bottom screen at least twice its size where that is possible; stacked: the top screen above the bottom one, as the console holds them; side_by_side: the top screen left of the bottom one; large_top and large_bottom: that screen at twice the size and the other one small beside or below it; top_only and bottom_only: one screen, swapped with the Swap screens control",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: [
                    "auto",
                    "stacked",
                    "side_by_side",
                    "large_top",
                    "large_bottom",
                    "top_only",
                    "bottom_only",
                ]
                .map(String::from)
                .into(),
            },
        ),
        s(
            "video.nds_displays",
            "Displays",
            "What the displays show when the device has a second one. auto: the top screen on the first display and the bottom screen on the second; swapped: the other way round; top_both and bottom_both: that screen on both displays; single: only the first display, with the screen layout",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "swapped", "top_both", "bottom_both", "single"]
                    .map(String::from)
                    .into(),
            },
        ),
        s(
            "video.nds_integer_scale",
            "Whole-number scaling",
            "Scales the screens by the largest whole multiple that fits the window, so every pixel keeps its size. Off: they fill the window with their shape kept",
            Value::Bool(false),
            SettingKind::Bool,
        ),
        s(
            "video.nds_lcd_response",
            "LCD response",
            "Shows a pixel that alternates between two colors on every frame as their mean, the way the console's slow LCD does, so a game's flicker transparency looks steady. A pixel that moves or changes for good is not touched. Off: every frame is shown as drawn",
            Value::Bool(true),
            SettingKind::Bool,
        ),
    ]
}

/// The settings of the on-screen controller (`runtime/core/touchpad.c`).
/// The editor's saved places (`input.touch_pad.layout.*`) are not listed.
#[must_use]
pub fn touch_pad_settings() -> Vec<Setting> {
    let s = |key: &str, label: &str, description: &str, default: Value, kind| Setting {
        key: key.into(),
        label: label.into(),
        description: description.into(),
        apply: Apply::Live,
        default,
        kind,
    };
    vec![
        s(
            "input.touch_pad.show",
            "Show",
            "Automatic shows the on-screen controller only while no controller is connected",
            Value::String("auto".into()),
            SettingKind::Enum {
                choices: ["auto", "always", "never"].map(String::from).into(),
            },
        ),
        s(
            "input.touch_pad.opacity",
            "Opacity",
            "How see-through the buttons are",
            Value::from(45),
            SettingKind::Int {
                min: 20,
                max: 100,
                step: 1,
            },
        ),
        s(
            "input.touch_pad.size",
            "Button size",
            "Bigger buttons are easier to hit",
            Value::from(100),
            SettingKind::Int {
                min: 70,
                max: 150,
                step: 1,
            },
        ),
        s(
            "input.touch_pad.haptics",
            "Vibrate on press",
            "A short tick when a button goes down",
            Value::Bool(true),
            SettingKind::Bool,
        ),
    ]
}

/// The `settings` section of the self-description for a game's settings.
#[must_use]
pub fn section(game: Vec<Setting>) -> relift_info::Settings {
    relift_info::Settings {
        runtime: runtime_settings()
            .into_iter()
            .chain(touch_pad_settings())
            .collect(),
        game,
    }
}

/// [`section`] for a Game Boy or Game Boy Color game, whose runtime also
/// declares [`gb_settings`].
#[must_use]
pub fn gb_section(game: Vec<Setting>) -> relift_info::Settings {
    let mut s = section(game);
    s.runtime.extend(gb_settings());
    s
}

/// [`section`] for a Nintendo DS game, whose runtime also declares
/// [`nds_settings`].
#[must_use]
pub fn nds_section(game: Vec<Setting>) -> relift_info::Settings {
    let mut s = section(game);
    s.runtime.extend(nds_settings());
    s
}

fn text(t: &toml::Table, key: &str, what: &str) -> Result<String, BoxError> {
    match t.get(key) {
        None => Ok(String::new()),
        Some(toml::Value::String(s)) => Ok(s.clone()),
        Some(_) => Err(format!("{what}: {key} must be a string").into()),
    }
}

fn int(t: &toml::Table, key: &str, what: &str) -> Result<Option<i64>, BoxError> {
    match t.get(key) {
        None => Ok(None),
        Some(toml::Value::Integer(i)) => Ok(Some(*i)),
        Some(_) => Err(format!("{what}: {key} must be an integer").into()),
    }
}

fn float(t: &toml::Table, key: &str, what: &str) -> Result<Option<f64>, BoxError> {
    match t.get(key) {
        None => Ok(None),
        Some(toml::Value::Float(f)) if f.is_finite() => Ok(Some(*f)),
        Some(toml::Value::Integer(i)) => Ok(Some(*i as f64)),
        Some(_) => Err(format!("{what}: {key} must be a finite number").into()),
    }
}

fn setting(key: &str, t: &toml::Table) -> Result<Setting, BoxError> {
    let what = format!("settings.{key}");
    if !key_ok(key) {
        return Err(format!(
            "{what}: a key is lowercase letters, digits, '_' and '.', starting with a letter"
        )
        .into());
    }
    if key.starts_with("bind.") {
        return Err(format!("{what}: keys starting with bind. belong to the input ports").into());
    }
    let known: &[&str] = &[
        "type",
        "label",
        "description",
        "apply",
        "default",
        "min",
        "max",
        "step",
        "choices",
    ];
    if let Some(k) = t.keys().find(|k| !known.contains(&k.as_str())) {
        return Err(format!("{what}: unknown field {k}").into());
    }
    let apply = match text(t, "apply", &what)?.as_str() {
        "" | "start" => Apply::Start,
        "live" => return Err(format!("{what}: apply = \"live\" is not supported yet").into()),
        o => return Err(format!("{what}: apply must be \"start\", not {o:?}").into()),
    };
    let ty = text(t, "type", &what)?;
    let (kind, default) = match ty.as_str() {
        "bool" => match t.get("default") {
            None => (SettingKind::Bool, Value::Bool(false)),
            Some(toml::Value::Boolean(b)) => (SettingKind::Bool, Value::Bool(*b)),
            Some(_) => return Err(format!("{what}: the default of a bool is true or false").into()),
        },
        "int" => {
            let (min, max) = (int(t, "min", &what)?, int(t, "max", &what)?);
            let (Some(min), Some(max)) = (min, max) else {
                return Err(format!("{what}: an int needs min and max").into());
            };
            let step = int(t, "step", &what)?.unwrap_or(0);
            let d = int(t, "default", &what)?.unwrap_or(min);
            if min > max || step < 0 || d < min || d > max {
                return Err(format!(
                    "{what}: the range {min}..{max} is empty, the step is negative or the default {d} is outside it"
                )
                .into());
            }
            if step > 0 && (d - min) % step != 0 {
                return Err(format!("{what}: the default {d} is not on the step {step}").into());
            }
            (SettingKind::Int { min, max, step }, Value::from(d))
        }
        "float" => {
            let (min, max) = (float(t, "min", &what)?, float(t, "max", &what)?);
            let (Some(min), Some(max)) = (min, max) else {
                return Err(format!("{what}: a float needs min and max").into());
            };
            let step = float(t, "step", &what)?.unwrap_or(0.0);
            let d = float(t, "default", &what)?.unwrap_or(min);
            if min > max || step < 0.0 || d < min || d > max {
                return Err(format!(
                    "{what}: the range {min}..{max} is empty, the step is negative or the default {d} is outside it"
                )
                .into());
            }
            (
                SettingKind::Float { min, max, step },
                serde_json::Number::from_f64(d).map_or(Value::Null, Value::Number),
            )
        }
        "enum" => {
            let Some(toml::Value::Array(a)) = t.get("choices") else {
                return Err(format!("{what}: an enum needs a list of choices").into());
            };
            let choices: Vec<String> = a
                .iter()
                .map(|v| v.as_str().map(str::to_string))
                .collect::<Option<_>>()
                .ok_or_else(|| format!("{what}: choices are strings"))?;
            if choices.is_empty() || choices.iter().collect::<BTreeSet<_>>().len() != choices.len()
            {
                return Err(format!("{what}: choices are not empty and not repeated").into());
            }
            let d = if t.contains_key("default") {
                text(t, "default", &what)?
            } else {
                choices[0].clone()
            };
            if !choices.contains(&d) {
                return Err(format!("{what}: the default {d:?} is not one of the choices").into());
            }
            (SettingKind::Enum { choices }, Value::String(d))
        }
        "string" => (
            SettingKind::String,
            Value::String(text(t, "default", &what)?),
        ),
        "key_binding" => {
            let d = text(t, "default", &what)?;
            if let Err(e) = relift_input::HostInput::parse_list(&d) {
                return Err(
                    format!("{what}: the default {d:?} is not a list of host inputs: {e}").into(),
                );
            }
            (SettingKind::KeyBinding, Value::String(d))
        }
        "" => return Err(format!("{what}: no type").into()),
        o => {
            return Err(format!(
                "{what}: type {o:?} is not bool, int, float, enum, string or key_binding"
            )
            .into());
        }
    };
    Ok(Setting {
        key: key.to_string(),
        label: text(t, "label", &what)?,
        description: text(t, "description", &what)?,
        apply,
        default,
        kind,
    })
}

/// The settings of a relift.toml text, in key order.
///
/// # Errors
///
/// The text is not TOML, or a `[settings.<key>]` table is malformed.
pub fn parse(game_toml: &str) -> Result<Vec<Setting>, BoxError> {
    let doc: toml::Table = game_toml.parse().map_err(|e| format!("relift.toml: {e}"))?;
    let Some(settings) = doc.get("settings") else {
        return Ok(Vec::new());
    };
    let toml::Value::Table(settings) = settings else {
        return Err("relift.toml: settings must be a table of settings".into());
    };
    settings
        .iter()
        .map(|(k, v)| match v {
            toml::Value::Table(t) => setting(k, t),
            _ => Err(format!("settings.{k} must be a table").into()),
        })
        .collect()
}

/// The settings of the relift.toml at `path`; none when the file is absent.
///
/// # Errors
///
/// The file cannot be read or [`parse`] fails.
pub fn read(path: &Path) -> Result<Vec<Setting>, BoxError> {
    match std::fs::read_to_string(path) {
        Ok(t) => parse(&t),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(format!("{}: {e}", path.display()).into()),
    }
}

fn c_str(s: &str) -> String {
    let mut o = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' => o.push_str("\\\""),
            b'\\' => o.push_str("\\\\"),
            b'\n' => o.push_str("\\n"),
            0x20..=0x7e => o.push(b as char),
            _ => {
                let _ = write!(o, "\\{b:03o}");
            }
        }
    }
    o.push('"');
    o
}

/// The C file that declares `settings` to the runtime before `main` runs.
#[must_use]
pub fn generate_c(settings: &[Setting]) -> String {
    let mut s = String::from(
        "/* Generated by relift: the game's settings. */\n#include <relift/settings.h>\n\n",
    );
    for (i, d) in settings.iter().enumerate() {
        if let SettingKind::Enum { choices } = &d.kind {
            let names: Vec<String> = choices.iter().map(|c| c_str(c)).collect();
            let _ = writeln!(
                s,
                "static const char *const rl_game_choices_{i}[] = {{ {} }};",
                names.join(", ")
            );
        }
    }
    s.push_str("\nstatic const rl_setting_def rl_game_settings[] = {\n");
    for (i, d) in settings.iter().enumerate() {
        let ty = match d.kind {
            SettingKind::Bool => "RL_SETTING_BOOL",
            SettingKind::Int { .. } => "RL_SETTING_INT",
            SettingKind::Float { .. } => "RL_SETTING_FLOAT",
            SettingKind::Enum { .. } => "RL_SETTING_ENUM",
            SettingKind::String => "RL_SETTING_STRING",
            SettingKind::KeyBinding => "RL_SETTING_BINDING",
        };
        let _ = write!(
            s,
            "    {{ .key = {}, .type = {ty}, .label = {}, .description = {}",
            c_str(&d.key),
            c_str(&d.label),
            c_str(&d.description)
        );
        match &d.kind {
            SettingKind::Bool => {
                let _ = write!(
                    s,
                    ", .def_int = {}",
                    i32::from(d.default == Value::Bool(true))
                );
            }
            SettingKind::Int { min, max, step } => {
                let _ = write!(
                    s,
                    ", .def_int = {}, .min_int = {min}, .max_int = {max}, .step_int = {step}",
                    d.default.as_i64().unwrap_or(*min)
                );
            }
            SettingKind::Float { min, max, step } => {
                let _ = write!(
                    s,
                    ", .def_float = {:?}, .min_float = {min:?}, .max_float = {max:?}, .step_float = {step:?}",
                    d.default.as_f64().unwrap_or(*min)
                );
            }
            SettingKind::Enum { choices } => {
                let idx = choices
                    .iter()
                    .position(|c| Some(c.as_str()) == d.default.as_str())
                    .unwrap_or(0);
                let _ = write!(
                    s,
                    ", .def_int = {idx}, .choices = rl_game_choices_{i}, .nchoices = {}",
                    choices.len()
                );
            }
            SettingKind::String => {
                let _ = write!(
                    s,
                    ", .def_str = {}",
                    c_str(d.default.as_str().unwrap_or(""))
                );
            }
            SettingKind::KeyBinding => {
                let _ = write!(
                    s,
                    ", .def_str = {}",
                    c_str(d.default.as_str().unwrap_or(""))
                );
            }
        }
        s.push_str(" },\n");
    }
    if settings.is_empty() {
        s.push_str("    { .key = \"\" },\n");
    }
    let _ = write!(
        s,
        "}};\n\n__attribute__((constructor)) static void rl_game_declare_settings(void)\n{{\n    rl_settings_declare(rl_game_settings, {});\n}}\n",
        settings.len()
    );
    s
}

/// A `relift_setting_*` read in an override source.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Read {
    /// The 1-based line.
    pub line: usize,
    /// The function name.
    pub func: String,
    /// The key literal, or `None` when the argument is not a string literal.
    pub key: Option<String>,
}

/// Every `relift_setting_*` call in `source`, skipping comments.
#[must_use]
pub fn reads_in(source: &str) -> Vec<Read> {
    let b = source.as_bytes();
    let mut out = Vec::new();
    let (mut i, mut line) = (0, 1);
    while i < b.len() {
        match b[i] {
            b'\n' => {
                line += 1;
                i += 1;
            }
            b'/' if b.get(i + 1) == Some(&b'/') => {
                while i < b.len() && b[i] != b'\n' {
                    i += 1;
                }
            }
            b'/' if b.get(i + 1) == Some(&b'*') => {
                i += 2;
                while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                    line += usize::from(b[i] == b'\n');
                    i += 1;
                }
                i += 2;
            }
            b'"' => {
                i += 1;
                while i < b.len() && b[i] != b'"' && b[i] != b'\n' {
                    i += if b[i] == b'\\' { 2 } else { 1 };
                }
                i += 1;
            }
            c if c == b'_' || c.is_ascii_alphabetic() => {
                let start = i;
                while i < b.len() && (b[i] == b'_' || b[i].is_ascii_alphanumeric()) {
                    i += 1;
                }
                let word = &source[start..i];
                if !word.starts_with("relift_setting_") || !READS.iter().any(|(n, _)| *n == word) {
                    continue;
                }
                let mut j = i;
                while j < b.len() && b[j].is_ascii_whitespace() {
                    j += 1;
                }
                if b.get(j) != Some(&b'(') {
                    continue;
                }
                j += 1;
                while j < b.len() && b[j].is_ascii_whitespace() {
                    j += 1;
                }
                let key = if b.get(j) == Some(&b'"') {
                    let k = j + 1;
                    let mut e = k;
                    while e < b.len() && b[e] != b'"' && b[e] != b'\\' && b[e] != b'\n' {
                        e += 1;
                    }
                    let mut after = e + 1;
                    while after < b.len() && b[after].is_ascii_whitespace() {
                        after += 1;
                    }
                    let plain =
                        b.get(e) == Some(&b'"') && matches!(b.get(after), Some(b',' | b')'));
                    plain.then(|| source[k..e].to_string())
                } else {
                    None
                };
                out.push(Read {
                    line,
                    func: word.to_string(),
                    key,
                });
            }
            _ => i += 1,
        }
    }
    out
}

/// Checks the reads of one source against `settings`; returns one message
/// per problem, each starting `file:line`.
#[must_use]
pub fn check_source(file: &str, source: &str, settings: &[Setting]) -> Vec<String> {
    let mut errors = Vec::new();
    for r in reads_in(source) {
        let Some(key) = &r.key else {
            errors.push(format!(
                "{file}:{}: {} needs a string literal key, so the build can check it",
                r.line, r.func
            ));
            continue;
        };
        let want = READS
            .iter()
            .find(|(n, _)| *n == r.func)
            .map_or("", |(_, t)| *t);
        match settings.iter().find(|s| &s.key == key) {
            None => {
                let near = settings
                    .iter()
                    .map(|s| s.key.as_str())
                    .min_by_key(|k| distance(k, key))
                    .filter(|k| distance(k, key) <= 2);
                let hint = near.map_or(String::new(), |k| format!("; did you mean {k:?}?"));
                errors.push(format!(
                    "{file}:{}: {} reads {key:?}, which relift.toml does not declare{hint}",
                    r.line, r.func
                ));
            }
            Some(s) if type_name(&s.kind) != want => errors.push(format!(
                "{file}:{}: {} reads {key:?}, which relift.toml declares as {}",
                r.line,
                r.func,
                type_name(&s.kind)
            )),
            Some(_) => {}
        }
    }
    errors
}

fn distance(a: &str, b: &str) -> usize {
    let (a, b): (Vec<char>, Vec<char>) = (a.chars().collect(), b.chars().collect());
    let mut row: Vec<usize> = (0..=b.len()).collect();
    for (i, ca) in a.iter().enumerate() {
        let mut prev = row[0];
        row[0] = i + 1;
        for (j, cb) in b.iter().enumerate() {
            let cur = row[j + 1];
            row[j + 1] = (prev + usize::from(ca != cb)).min(row[j] + 1).min(cur + 1);
            prev = cur;
        }
    }
    row[b.len()]
}

fn sources_under(dir: &Path, out: &mut Vec<PathBuf>) -> std::io::Result<()> {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return Ok(());
    };
    let mut entries: Vec<PathBuf> = rd.filter_map(|e| e.ok().map(|e| e.path())).collect();
    entries.sort();
    for p in entries {
        if p.is_dir() {
            sources_under(&p, out)?;
        } else if p
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| matches!(e, "c" | "h" | "cc" | "cpp" | "cxx" | "hpp"))
        {
            out.push(p);
        }
    }
    Ok(())
}

/// Checks every C and C++ source under `dir` (the game's overrides folder);
/// a missing folder has nothing to check.
///
/// # Errors
///
/// One error listing every misspelled, undeclared or mistyped key.
pub fn check_dir(dir: &Path, settings: &[Setting]) -> Result<(), BoxError> {
    let mut files = Vec::new();
    sources_under(dir, &mut files)?;
    let mut errors = Vec::new();
    for f in &files {
        let src = std::fs::read_to_string(f).map_err(|e| format!("{}: {e}", f.display()))?;
        errors.extend(check_source(&f.display().to_string(), &src, settings));
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n").into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_game_boy_section_adds_the_colour_correction_after_the_runtime_ones() {
        let gb = gb_section(Vec::new());
        let plain = section(Vec::new());
        assert_eq!(gb.runtime.len(), plain.runtime.len() + 1);
        assert_eq!(gb.runtime[..plain.runtime.len()], plain.runtime[..]);
        let last = gb.runtime.last().unwrap();
        assert_eq!(last.key, "video.gb_color_correction");
        assert_eq!(last.default, Value::Bool(true));
    }

    #[test]
    fn a_ds_section_adds_the_screen_settings_after_the_runtime_ones() {
        let nds = nds_section(Vec::new());
        let plain = section(Vec::new());
        assert_eq!(nds.runtime.len(), plain.runtime.len() + 4);
        assert_eq!(nds.runtime[..plain.runtime.len()], plain.runtime[..]);
        let keys: Vec<&str> = nds.runtime[plain.runtime.len()..]
            .iter()
            .map(|s| s.key.as_str())
            .collect();
        assert_eq!(
            keys,
            [
                "video.nds_layout",
                "video.nds_displays",
                "video.nds_integer_scale",
                "video.nds_lcd_response"
            ]
        );
    }

    const GOOD: &str = r#"
[game]
guest = "switch"

[settings.fast_text]
type = "bool"
label = "Fast text"
default = true

[settings.scale]
type = "int"
min = 1
max = 9
step = 2
default = 3

[settings.gain]
type = "float"
min = 0
max = 1
step = 0.25
default = 0.5

[settings.mode]
type = "enum"
choices = ["off", "low", "high"]
default = "low"

[settings.name]
type = "string"
default = "a\"b"

[settings.jump]
type = "key_binding"
default = "gamepad:south,key:z"
"#;

    fn err(t: &str) -> String {
        parse(t).expect_err("must fail").to_string()
    }

    #[test]
    fn parses_every_type() {
        let s = parse(GOOD).unwrap();
        assert_eq!(s.len(), 6);
        let by = |k: &str| s.iter().find(|x| x.key == k).unwrap();
        assert_eq!(by("fast_text").default, Value::Bool(true));
        assert_eq!(
            by("scale").kind,
            SettingKind::Int {
                min: 1,
                max: 9,
                step: 2
            }
        );
        assert_eq!(by("gain").default, serde_json::json!(0.5));
        assert_eq!(by("mode").default, "low");
        assert_eq!(by("jump").default, "gamepad:south,key:z");
        assert_eq!(by("fast_text").label, "Fast text");
    }

    #[test]
    fn runtime_settings_match_the_runtime_declarations() {
        let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
        let mut c = String::new();
        for f in ["core/settings.c", "guests/switch/settings.c"] {
            c.push_str(&std::fs::read_to_string(rt.join(f)).unwrap());
        }
        let declared: Vec<String> = c
            .split(".key = \"")
            .skip(1)
            .map(|r| r.split('"').next().unwrap().to_string())
            .collect();
        let listed: Vec<String> = runtime_settings().into_iter().map(|s| s.key).collect();
        assert_eq!(declared, listed);
        assert!(runtime_settings().iter().all(|s| key_ok(&s.key)));
        assert_eq!(section(Vec::new()).runtime.len(), 30);
    }

    #[test]
    fn touch_pad_settings_match_the_runtime_declarations() {
        let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
        let c = std::fs::read_to_string(rt.join("core/touchpad.c")).unwrap();
        let declared: Vec<String> = c
            .split(".key = \"")
            .skip(1)
            .map(|r| r.split('"').next().unwrap().to_string())
            .collect();
        let listed: Vec<String> = touch_pad_settings().into_iter().map(|s| s.key).collect();
        assert_eq!(declared, listed);
        for (s, label) in
            touch_pad_settings()
                .iter()
                .zip(["Show", "Opacity", "Button size", "Vibrate on press"])
        {
            assert!(c.contains(&format!(".label = \"{label}\"")), "{}", s.key);
            assert!(
                c.contains(&format!(".description = \"{}\"", s.description)),
                "{}",
                s.key
            );
        }
    }

    #[test]
    fn no_settings_table_is_no_settings() {
        assert!(parse("[game]\nguest = \"switch\"\n").unwrap().is_empty());
    }

    #[test]
    fn rejects_bad_declarations() {
        let cases = [
            ("[settings.A]\ntype = \"bool\"", "lowercase"),
            ("[settings.a]\n", "no type"),
            ("[settings.a]\ntype = \"blob\"", "is not bool"),
            (
                "[settings.a]\ntype = \"bool\"\ndefault = 1",
                "true or false",
            ),
            ("[settings.a]\ntype = \"int\"\nmin = 1", "needs min and max"),
            ("[settings.a]\ntype = \"int\"\nmin = 5\nmax = 1", "empty"),
            (
                "[settings.a]\ntype = \"int\"\nmin = 1\nmax = 5\ndefault = 6",
                "outside",
            ),
            (
                "[settings.a]\ntype = \"int\"\nmin = 1\nmax = 9\nstep = 2\ndefault = 2",
                "not on the step",
            ),
            (
                "[settings.a]\ntype = \"float\"\nmin = 0\nmax = 1\ndefault = 2.0",
                "outside",
            ),
            ("[settings.a]\ntype = \"enum\"", "list of choices"),
            ("[settings.a]\ntype = \"enum\"\nchoices = []", "not empty"),
            (
                "[settings.a]\ntype = \"enum\"\nchoices = [\"x\", \"x\"]",
                "not repeated",
            ),
            (
                "[settings.a]\ntype = \"enum\"\nchoices = [\"x\"]\ndefault = \"y\"",
                "not one of",
            ),
            (
                "[settings.a]\ntype = \"key_binding\"\ndefault = \"p1:a\"",
                "list of host inputs",
            ),
            (
                "[settings.\"bind.p1.a\"]\ntype = \"key_binding\"",
                "belong to the input ports",
            ),
            (
                "[settings.a]\ntype = \"bool\"\nlabl = \"x\"",
                "unknown field labl",
            ),
            (
                "[settings.a]\ntype = \"bool\"\napply = \"live\"",
                "not supported",
            ),
            ("[settings]\na = 1", "must be a table"),
            ("settings = 1", "table of settings"),
        ];
        for (t, want) in cases {
            let e = err(t);
            assert!(e.contains(want), "{t:?}: {e}");
        }
    }

    #[test]
    fn generated_c_declares_every_setting() {
        let c = generate_c(&parse(GOOD).unwrap());
        for want in [
            ".key = \"fast_text\", .type = RL_SETTING_BOOL",
            ".def_int = 3, .min_int = 1, .max_int = 9, .step_int = 2",
            ".def_float = 0.5, .min_float = 0.0, .max_float = 1.0, .step_float = 0.25",
            ".def_int = 1, .choices = rl_game_choices_3, .nchoices = 3",
            ".def_str = \"a\\\"b\"",
            ".def_str = \"gamepad:south,key:z\"",
            "rl_settings_declare(rl_game_settings, 6)",
            "__attribute__((constructor))",
        ] {
            assert!(c.contains(want), "{want} not in\n{c}");
        }
        assert!(generate_c(&[]).contains("rl_settings_declare(rl_game_settings, 0)"));
    }

    #[test]
    fn finds_reads_and_skips_comments_and_strings() {
        let src = "int a = relift_setting_int(\"scale\");\n// relift_setting_bool(\"no\")\n/* relift_setting_bool(\"no2\") */\nconst char *s = \"relift_setting_bool(\\\"no3\\\")\";\nbool b = relift_setting_bool (\"fast_text\");\nbool c = relift_setting_bool(key);\n";
        let r = reads_in(src);
        assert_eq!(r.len(), 3);
        assert_eq!(r[0].key.as_deref(), Some("scale"));
        assert_eq!(r[0].line, 1);
        assert_eq!(r[1].key.as_deref(), Some("fast_text"));
        assert_eq!(r[1].line, 5);
        assert_eq!(r[2].key, None);
    }

    #[test]
    fn a_misspelled_or_mistyped_key_is_an_error() {
        let s = parse(GOOD).unwrap();
        let ok = "relift_setting_bool(\"fast_text\"); relift_setting_int(\"scale\");";
        assert!(check_source("a.c", ok, &s).is_empty());
        let e = check_source("a.c", "x = relift_setting_bool(\"fast_txt\");", &s);
        assert_eq!(e.len(), 1);
        assert!(e[0].starts_with("a.c:1:") && e[0].contains("did you mean \"fast_text\""));
        let e = check_source("a.c", "x = relift_setting_int(\"fast_text\");", &s);
        assert!(e[0].contains("declares as bool"));
        let e = check_source("a.c", "x = relift_setting_int(k);", &s);
        assert!(e[0].contains("string literal"));
        let e = check_source("a.c", "x = relift_setting_bool(\"zzzzzzzz\");", &s);
        assert!(!e[0].contains("did you mean"));
    }

    #[test]
    fn check_dir_walks_the_overrides_folder() {
        let root = std::env::temp_dir().join(format!("relift-gs-{}", std::process::id()));
        let sub = root.join("deep");
        std::fs::create_dir_all(&sub).unwrap();
        relift_cache::write_if_changed(
            &root.join("a.c"),
            "relift_setting_bool(\"fast_text\");".as_bytes(),
        )
        .unwrap();
        relift_cache::write_if_changed(
            &sub.join("b.cpp"),
            "relift_setting_bool(\"fast_tex\");".as_bytes(),
        )
        .unwrap();
        relift_cache::write_if_changed(
            &sub.join("note.txt"),
            "relift_setting_bool(\"zz\");".as_bytes(),
        )
        .unwrap();
        let s = parse(GOOD).unwrap();
        let e = check_dir(&root, &s).expect_err("misspelled").to_string();
        assert!(e.contains("b.cpp:1") && !e.contains("note.txt") && !e.contains("a.c:"));
        std::fs::remove_file(sub.join("b.cpp")).unwrap();
        assert!(check_dir(&root, &s).is_ok());
        assert!(check_dir(&root.join("missing"), &s).is_ok());
        std::fs::remove_dir_all(&root).unwrap();
    }
}
