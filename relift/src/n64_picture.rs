//! The picture settings of a Nintendo 64 program (`n64.picture.*`, declared
//! by `runtime/guests/n64/picture.c`) and the developer's defaults for them
//! in `game.toml`. The declared defaults are the console's picture; the
//! developer picks another as the program's default, and a run can still
//! override every key.
//!
//! ```toml
//! [n64.picture]
//! upscale = "4x"
//! downscale_steps = 2
//! divot = false
//! ```
//!
//! Setting any key but `mode` makes the default mode `custom`; `mode =
//! "console"` keeps the console's picture whatever the other keys say.

use relift_info::{Apply, Setting, SettingKind};
use serde_json::Value;

use crate::import::BoxError;

const PREFIX: &str = "n64.picture.";

/// The picture settings with the console's defaults, in the order the
/// runtime declares them. A test keeps this list equal to the declarations.
#[must_use]
pub fn settings() -> Vec<Setting> {
    let s = |key: &str, label: &str, description: &str, default: Value, kind| Setting {
        key: format!("{PREFIX}{key}"),
        label: label.into(),
        description: description.into(),
        apply: Apply::Start,
        default,
        kind,
    };
    let choices = |c: &[&str]| SettingKind::Enum {
        choices: c.iter().map(|s| (*s).to_string()).collect(),
    };
    let b = |key: &str, label: &str, description: &str, default: bool| {
        s(
            key,
            label,
            description,
            Value::Bool(default),
            SettingKind::Bool,
        )
    };
    vec![
        s(
            "mode",
            "Picture",
            "Console: the console's picture at its own resolution, every other picture setting ignored. Custom: the picture the other picture settings describe",
            Value::String("console".into()),
            choices(&["console", "custom"]),
        ),
        s(
            "upscale",
            "Internal resolution",
            "The RDP draws at this multiple of the console's resolution",
            Value::String("1x".into()),
            choices(&["1x", "2x", "4x", "8x"]),
        ),
        s(
            "downscale_steps",
            "Downscale steps",
            "Halvings of an upscaled picture before it is shown; enough of them gives the console's resolution with the upscaled picture averaged into it",
            Value::from(0),
            SettingKind::Int {
                min: 0,
                max: 3,
                step: 1,
            },
        ),
        b(
            "super_sampled_readback",
            "Super-sampled read back",
            "The game reads back the upscaled framebuffer averaged to its own resolution, not one drawn at it",
            false,
        ),
        b(
            "readback_dither",
            "Dither the read back",
            "Dithers the averaged framebuffer a super-sampled read back gives the game",
            true,
        ),
        b(
            "native_tex_rect",
            "Texture rectangles at native resolution",
            "Draws texture rectangles without upscaling, as games that copy pixels one to one expect",
            true,
        ),
        b(
            "native_texture_lod",
            "Texture detail at native resolution",
            "Picks texture detail levels as at the console's resolution",
            false,
        ),
        b(
            "vi_aa",
            "Video anti-aliasing",
            "The video interface smooths polygon edges when the game asks",
            true,
        ),
        b(
            "vi_resample",
            "Video resampling",
            "The video interface scales the framebuffer bilinearly when the game asks, else by repeating pixels",
            true,
        ),
        b(
            "vi_serrate",
            "Interlacing",
            "The video interface interlaces when the game asks",
            true,
        ),
        b(
            "dither_filter",
            "Dither filter",
            "The video interface smooths the RDP's dither when the game asks",
            true,
        ),
        b(
            "divot",
            "Divot filter",
            "The video interface removes one-pixel gaps on edges when the game asks",
            true,
        ),
        b(
            "gamma_dither",
            "Gamma dither",
            "The video interface dithers its gamma when the game asks",
            true,
        ),
        s(
            "crop_overscan",
            "Overscan crop",
            "Lines cropped from the top and bottom, and a matching width from the sides",
            Value::from(0),
            SettingKind::Int {
                min: 0,
                max: 64,
                step: 1,
            },
        ),
        s(
            "deinterlace",
            "Deinterlacing",
            "Bob shows each field on its own; weave blends it with the field before",
            Value::String("bob".into()),
            choices(&["bob", "weave"]),
        ),
    ]
}

/// The value `v` of the setting `s` as the runtime's text, or why it does not fit.
fn value_text(s: &Setting, v: &toml::Value) -> Result<String, String> {
    match (&s.kind, v) {
        (SettingKind::Bool, toml::Value::Boolean(b)) => Ok(b.to_string()),
        (SettingKind::Int { min, max, .. }, toml::Value::Integer(i))
            if (*min..=*max).contains(i) =>
        {
            Ok(i.to_string())
        }
        (SettingKind::Int { min, max, .. }, _) => Err(format!("an integer from {min} to {max}")),
        (SettingKind::Enum { choices }, toml::Value::String(t)) if choices.contains(t) => {
            Ok(t.clone())
        }
        (SettingKind::Enum { choices }, _) => Err(format!("one of {}", choices.join(", "))),
        _ => Err("true or false".into()),
    }
}

/// The developer's picture defaults in `game_toml` (its `[n64.picture]`
/// table) as `name=value` texts for the program, in declaration order, with
/// `mode=custom` first when another key is set and `mode` is not.
///
/// # Errors
///
/// The text is not TOML, or the table names an unknown key or a value that
/// does not fit its setting.
pub fn defaults(game_toml: &str) -> Result<Vec<String>, BoxError> {
    let doc: toml::Table = game_toml.parse().map_err(|e| format!("game.toml: {e}"))?;
    let Some(table) = doc.get("n64").and_then(|n| n.get("picture")) else {
        return Ok(Vec::new());
    };
    let toml::Value::Table(table) = table else {
        return Err("game.toml: n64.picture must be a table".into());
    };
    let all = settings();
    for k in table.keys() {
        if !all.iter().any(|s| s.key[PREFIX.len()..] == *k) {
            let names: Vec<&str> = all.iter().map(|s| &s.key[PREFIX.len()..]).collect();
            return Err(format!(
                "game.toml: n64.picture.{k} is not a picture setting ({})",
                names.join(", ")
            )
            .into());
        }
    }
    let mut out = Vec::new();
    for s in &all {
        let name = &s.key[PREFIX.len()..];
        if let Some(v) = table.get(name) {
            let text = value_text(s, v)
                .map_err(|want| format!("game.toml: n64.picture.{name} must be {want}"))?;
            out.push(format!("{name}={text}"));
        }
    }
    if !out.is_empty() && !table.contains_key("mode") {
        out.insert(0, "mode=custom".into());
    }
    Ok(out)
}

/// [`settings`] with the developer's `defaults` (from [`defaults`]) as their defaults.
#[must_use]
pub fn with_defaults(defaults: &[String]) -> Vec<Setting> {
    let mut all = settings();
    for d in defaults {
        let Some((name, text)) = d.split_once('=') else {
            continue;
        };
        if let Some(s) = all.iter_mut().find(|s| s.key[PREFIX.len()..] == *name) {
            s.default = match s.kind {
                SettingKind::Bool => Value::Bool(text == "true"),
                SettingKind::Int { .. } => text.parse::<i64>().map_or(Value::Null, Value::from),
                _ => Value::String(text.into()),
            };
        }
    }
    all
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_settings_match_the_runtime_declarations() {
        let c = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../runtime/guests/n64/picture.c"),
        )
        .unwrap();
        let mut declared: Vec<String> = c
            .split(".key = \"")
            .skip(1)
            .map(|r| r.split('"').next().unwrap().to_string())
            .filter(|k| k != PREFIX)
            .collect();
        declared.extend(
            c.split("BOOL_DEF(\"")
                .skip(1)
                .map(|r| format!("{PREFIX}{}", r.split('"').next().unwrap())),
        );
        let mut listed: Vec<String> = settings().into_iter().map(|s| s.key).collect();
        declared.sort();
        listed.sort();
        assert_eq!(declared, listed);
        assert!(
            settings()
                .iter()
                .all(|s| crate::game_settings::key_ok(&s.key))
        );
    }

    #[test]
    fn no_table_keeps_the_console_picture() {
        assert!(defaults("[game]\nguest = \"n64\"\n").unwrap().is_empty());
    }

    #[test]
    fn a_table_gives_custom_defaults_in_declaration_order() {
        let d = defaults("[n64.picture]\ndivot = false\nupscale = \"4x\"\ndownscale_steps = 2\n")
            .unwrap();
        assert_eq!(
            d,
            [
                "mode=custom",
                "upscale=4x",
                "downscale_steps=2",
                "divot=false"
            ]
        );
        let s = with_defaults(&d);
        let by = |k: &str| {
            s.iter()
                .find(|s| s.key == format!("{PREFIX}{k}"))
                .unwrap()
                .default
                .clone()
        };
        assert_eq!(by("mode"), Value::String("custom".into()));
        assert_eq!(by("upscale"), Value::String("4x".into()));
        assert_eq!(by("downscale_steps"), Value::from(2));
        assert_eq!(by("divot"), Value::Bool(false));
        assert_eq!(by("vi_aa"), Value::Bool(true));
    }

    #[test]
    fn an_explicit_console_mode_stays() {
        let d = defaults("[n64.picture]\nmode = \"console\"\nupscale = \"2x\"\n").unwrap();
        assert_eq!(d, ["mode=console", "upscale=2x"]);
    }

    #[test]
    fn rejects_unknown_keys_and_bad_values() {
        let err = |t: &str| defaults(t).unwrap_err().to_string();
        assert!(err("[n64.picture]\nupscal = \"2x\"\n").contains("not a picture setting"));
        assert!(err("[n64.picture]\nupscale = \"3x\"\n").contains("one of 1x, 2x, 4x, 8x"));
        assert!(err("[n64.picture]\ndownscale_steps = 9\n").contains("from 0 to 3"));
        assert!(err("[n64.picture]\ndivot = 1\n").contains("true or false"));
        assert!(err("[n64]\npicture = 1\n").contains("must be a table"));
    }
}
