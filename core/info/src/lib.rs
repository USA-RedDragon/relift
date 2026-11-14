//! The self-description of a relift binary (design 006): one JSON document
//! in sections, stored in the binary, printed by `--relift-info` and read
//! back without running the binary.
//!
//! The types here are shared by every lane that fills a section. A reader
//! ignores sections and fields it does not know: unknown top-level sections
//! are kept in [`Info::other`], so a document survives a round trip.

pub mod embed;
pub mod section;

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// The version of the document's layout. A launcher that knows a higher
/// protocol than a binary's reads the binary's sections it knows.
pub const PROTOCOL: u32 = 1;

/// The `--relift-info` flag of a game binary.
pub const FLAG: &str = "--relift-info";

/// The whole document.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Info {
    /// The protocol and the relift that built the binary.
    #[serde(default)]
    pub relift: Relift,
    /// Which game this is.
    #[serde(default)]
    pub identity: Identity,
    /// What the binary must be given at run time.
    #[serde(default)]
    pub needs: Needs,
    /// The settings schema.
    #[serde(default)]
    pub settings: Settings,
    /// The input ports.
    #[serde(default)]
    pub input: Input,
    /// The link ports.
    #[serde(default)]
    pub link: Link,
    /// The mods built in and the kinds accepted at run time.
    #[serde(default)]
    pub mods: Mods,
    /// How saves are stored.
    #[serde(default)]
    pub save: Save,
    /// Sections this version does not know, kept as read.
    #[serde(flatten)]
    pub other: BTreeMap<String, Value>,
}

impl Info {
    /// A document with the current protocol and the given relift version.
    #[must_use]
    pub fn new(relift_version: &str) -> Info {
        Info {
            relift: Relift {
                protocol: PROTOCOL,
                version: relift_version.to_string(),
            },
            ..Info::default()
        }
    }

    /// The document as stored: compact JSON and a newline.
    ///
    /// # Panics
    ///
    /// Never: every type here serializes.
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut b = serde_json::to_vec(self).expect("an Info serializes");
        b.push(b'\n');
        b
    }

    /// Reads a document.
    ///
    /// # Errors
    ///
    /// The bytes are not JSON or a known section has the wrong shape.
    pub fn from_bytes(bytes: &[u8]) -> Result<Info, serde_json::Error> {
        serde_json::from_slice(bytes)
    }
}

/// The `relift` section.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Relift {
    /// The document protocol version ([`PROTOCOL`]).
    pub protocol: u32,
    /// The relift version that built the binary.
    pub version: String,
}

/// The `identity` section.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Identity {
    /// The guest platform: `switch`, `gb`, `gbc`, `gba`, `nds`.
    pub platform: String,
    /// The title id (Switch) or the cartridge code (GBA, GBC), when there is
    /// one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub title_id: Option<String>,
    /// The title per language, keyed by language name (`AmericanEnglish`)
    /// or `default`.
    #[serde(default)]
    pub names: BTreeMap<String, String>,
    /// The publisher per language, as `names`.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub publishers: BTreeMap<String, String>,
    /// The icon, when the game has one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon: Option<Icon>,
    /// The game's own version string or number.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
    /// The code modules, by build id.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub modules: Vec<ModuleId>,
    /// The game repo that built the binary.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub repo: Option<Repo>,
    /// The patch, for a patched ROM.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub patch: Option<Patch>,
    /// The original's identity, for a patched ROM.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub base: Option<Box<Identity>>,
    /// Whether the patch's declared base did not match the ROM it was
    /// applied to.
    #[serde(default)]
    pub base_mismatch: bool,
}

/// An icon, embedded.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Icon {
    /// The image format: `jpeg`, `png`, `raw`.
    pub format: String,
    /// The image bytes in base64.
    pub data: String,
}

/// A code module of the game.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModuleId {
    /// The module name.
    pub name: String,
    /// Its build id, lowercase hex.
    pub build_id: String,
}

/// The game repo's name and version.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Repo {
    /// The repo or project name.
    pub name: String,
    /// The version the game repo declares.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
}

/// A ROM patch applied at build time.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Patch {
    /// The patch name.
    pub name: String,
    /// The format: `ips`, `ups`, `bps`, `xdelta`.
    pub kind: String,
    /// SHA-256 of the patch file, lowercase hex.
    pub hash: String,
}

/// The `needs` section.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Needs {
    /// The romfs tree, when the game reads one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub romfs: Option<TreeNeed>,
    /// Whether the game reads a DLC folder.
    #[serde(default)]
    pub dlc: bool,
    /// Whether the binary needs a save folder.
    #[serde(default)]
    pub save_folder: bool,
}

/// A directory tree the binary expects.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TreeNeed {
    /// The tree hash: SHA-256 over the sorted relative paths and the
    /// SHA-256 of every file, lowercase hex.
    pub hash: String,
}

/// The `settings` section: relift's settings first, then the game's.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Settings {
    /// Settings relift defines.
    #[serde(default)]
    pub runtime: Vec<Setting>,
    /// Settings the game repo declares.
    #[serde(default)]
    pub game: Vec<Setting>,
}

/// When a setting takes effect.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Apply {
    /// At program start.
    #[default]
    Start,
    /// While the program runs.
    Live,
}

/// One setting.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Setting {
    /// The key.
    pub key: String,
    /// The label a launcher shows.
    #[serde(default)]
    pub label: String,
    /// The longer description.
    #[serde(default)]
    pub description: String,
    /// When it applies.
    #[serde(default)]
    pub apply: Apply,
    /// The default value.
    pub default: Value,
    /// The type and its limits.
    #[serde(flatten)]
    pub kind: SettingKind,
}

/// A setting's type.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum SettingKind {
    /// On or off.
    Bool,
    /// A whole number.
    Int {
        /// The least value.
        min: i64,
        /// The greatest value.
        max: i64,
        /// The step.
        step: i64,
    },
    /// A real number.
    Float {
        /// The least value.
        min: f64,
        /// The greatest value.
        max: f64,
        /// The step.
        step: f64,
    },
    /// One of a list.
    Enum {
        /// The allowed values.
        choices: Vec<String>,
    },
    /// Text.
    String,
    /// A binding to a port's control; the value is `{port, control}`.
    KeyBinding,
}

/// The `input` section.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Input {
    /// Every port, native ones first.
    #[serde(default)]
    pub ports: Vec<Port>,
}

/// One input port.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Port {
    /// The port number, from 0.
    pub port: u32,
    /// The controller kind (`GBA pad`).
    pub kind: String,
    /// The controls.
    pub controls: Vec<Control>,
    /// Whether the game repo or a mod added this port or its controls.
    #[serde(default)]
    pub extra: bool,
}

/// One control of a port.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Control {
    /// The control id (`a`, `dpad_up`, `left_stick`).
    pub id: String,
    /// The label a launcher shows.
    pub label: String,
}

/// The `link` section: declared only for now.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Link {
    /// The game's link ports.
    #[serde(default)]
    pub ports: Vec<LinkPort>,
}

/// One link port.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LinkPort {
    /// `cable` or `wireless`.
    pub kind: String,
    /// The label.
    pub label: String,
}

/// The `mods` section.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Mods {
    /// Code mods built in, in order.
    #[serde(default)]
    pub code: Vec<CodeMod>,
    /// Setting keys of the bundled mods.
    #[serde(default)]
    pub bundled: Vec<String>,
    /// The run-time mod kinds this binary accepts.
    #[serde(default)]
    pub accepts: Vec<ModKind>,
}

/// A code mod built in.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CodeMod {
    /// The mod name.
    pub name: String,
    /// SHA-256 of the mod's files, lowercase hex.
    pub hash: String,
}

/// A kind of run-time mod and where it is expected.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModKind {
    /// `romfs`, `patch`, `cheat`.
    pub kind: String,
    /// The path pattern, relative to the mods folder.
    pub path: String,
}

/// The `save` section.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Save {
    /// `tree` (files), `raw` (battery RAM) or `none`.
    #[serde(default)]
    pub kind: String,
    /// The size in bytes, for a raw save.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub size: Option<u64>,
    /// The chip type, for a raw save.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chip: Option<String>,
    /// Where saves live, or `launcher` when the launcher passes the path.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub location: Option<String>,
    /// Whether the build's mods change the save format.
    #[serde(default)]
    pub changes_format: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_document_round_trips_byte_for_byte() {
        let mut info = Info::new("0.1.0");
        info.identity.platform = "gba".into();
        info.identity.base = Some(Box::new(Identity {
            platform: "gba".into(),
            ..Identity::default()
        }));
        info.settings.game.push(Setting {
            key: "speed".into(),
            label: "Speed".into(),
            description: String::new(),
            apply: Apply::Start,
            default: Value::from(2),
            kind: SettingKind::Int {
                min: 1,
                max: 4,
                step: 1,
            },
        });
        let bytes = info.to_bytes();
        let back = Info::from_bytes(&bytes).expect("parses");
        assert_eq!(back, info);
        assert_eq!(back.to_bytes(), bytes);
    }

    #[test]
    fn unknown_sections_and_fields_are_ignored_and_kept() {
        let doc = br#"{"relift":{"protocol":7,"version":"9","future":1},
            "identity":{"platform":"switch","names":{},"nonsense":true},
            "telemetry":{"a":[1,2]},"save":{"kind":"tree","next":"x"}}"#;
        let info = Info::from_bytes(doc).expect("parses");
        assert_eq!(info.relift.protocol, 7);
        assert_eq!(info.identity.platform, "switch");
        assert_eq!(info.save.kind, "tree");
        assert_eq!(info.other["telemetry"]["a"][1], 2);
        assert!(info.to_bytes().windows(9).any(|w| w == b"telemetry"));
    }

    #[test]
    fn every_setting_type_names_itself() {
        let kinds = [
            (SettingKind::Bool, "bool"),
            (SettingKind::String, "string"),
            (SettingKind::KeyBinding, "key_binding"),
            (SettingKind::Enum { choices: vec![] }, "enum"),
        ];
        for (kind, name) in kinds {
            let s = Setting {
                key: "k".into(),
                label: String::new(),
                description: String::new(),
                apply: Apply::Start,
                default: Value::Null,
                kind,
            };
            let v = serde_json::to_value(&s).unwrap();
            assert_eq!(v["type"], name);
            assert_eq!(v["apply"], "start");
            assert_eq!(serde_json::from_value::<Setting>(v).unwrap(), s);
        }
    }
}
