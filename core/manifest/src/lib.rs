//! The per-game tables of `relift.toml`: core keys plus a guest-owned section.
//!
//! Minimal for now: `relift import` writes a stub with the title's identity
//! and where the import landed. The reader comes with `relift build`.

/// The core keys of `relift.toml`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Game {
    /// The guest that owns the game (`switch`).
    pub guest: String,
    /// The title ID, 16 lowercase hex digits.
    pub title_id: String,
    /// The title version that was imported.
    pub version: u32,
    /// The import directory, as given to the importer.
    pub import_dir: String,
    /// The imported modules, by name and build ID.
    pub modules: Vec<ModuleRef>,
}

/// A module the game is made of.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModuleRef {
    /// The module name.
    pub name: String,
    /// The build ID, lowercase hex.
    pub build_id: String,
}

/// Quote a string as a TOML basic string.
fn quote(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 || c == '\x7f' => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

impl Game {
    /// The stub `relift.toml` text.
    pub fn to_toml(&self) -> String {
        let mut s = format!(
            "# The project file, written by relift import.\n\n[game]\nguest = {}\ntitle_id = \"{}\"\nversion = {}\nimport_dir = {}\n",
            quote(&self.guest),
            self.title_id,
            self.version,
            quote(&self.import_dir)
        );
        for m in &self.modules {
            s.push_str(&format!(
                "\n[[game.module]]\nname = {}\nbuild_id = \"{}\"\n",
                quote(&m.name),
                m.build_id
            ));
        }
        s
    }
}
