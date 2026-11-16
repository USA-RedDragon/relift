//! The hint list in `game.toml`: guest code addresses that a dev run
//! (`relift run`) missed and hot-loaded. `relift build` lifts each as a root,
//! so the next build has no miss there; `relift coverage` lists them.
//!
//! The list is the `[hints]` table, which relift owns and rewrites as a
//! whole at the end of the file; the rest of the file is kept as written:
//!
//! ```toml
//! [hints]
//! code = ["main+0x170c0"]
//! ```
//!
//! Each entry names a module and the offset from its load address, so a
//! hint stays valid while the module does.

use std::collections::BTreeSet;
use std::path::Path;

use crate::import::BoxError;

/// One code address a dev run missed.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Hint {
    /// The module name, as `modules.toml` gives it.
    pub module: String,
    /// The offset from the module's load address.
    pub offset: u64,
}

impl Hint {
    /// The entry text, `<module>+0x<offset>`.
    pub fn text(&self) -> String {
        format!("{}+{:#x}", self.module, self.offset)
    }

    /// Parses `<module>+0x<offset>`.
    ///
    /// # Errors
    ///
    /// Text of another shape.
    pub fn parse(s: &str) -> Result<Hint, BoxError> {
        let (module, off) = s
            .rsplit_once('+')
            .ok_or_else(|| format!("hint {s:?} is not <module>+0x<offset>"))?;
        let hex = off
            .strip_prefix("0x")
            .ok_or_else(|| format!("hint {s:?}: the offset needs a 0x prefix"))?;
        let offset =
            u64::from_str_radix(hex, 16).map_err(|e| format!("hint {s:?}: bad offset: {e}"))?;
        if module.is_empty() {
            return Err(format!("hint {s:?} names no module").into());
        }
        Ok(Hint {
            module: module.to_string(),
            offset,
        })
    }
}

/// The hints of the game.toml text `text`.
///
/// # Errors
///
/// Text that is not TOML, or a hint entry that does not parse.
pub fn parse(text: &str) -> Result<BTreeSet<Hint>, BoxError> {
    let table: toml::Table = text.parse()?;
    let Some(code) = table.get("hints").and_then(|h| h.get("code")) else {
        return Ok(BTreeSet::new());
    };
    code.as_array()
        .ok_or("hints.code is not an array")?
        .iter()
        .map(|v| Hint::parse(v.as_str().ok_or("hints.code holds a non-string")?))
        .collect()
}

/// The hints in the game.toml at `path`; none when the file has no list.
///
/// # Errors
///
/// A file that cannot be read or does not parse.
pub fn read(path: &Path) -> Result<BTreeSet<Hint>, BoxError> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    parse(&text).map_err(|e| format!("{}: {e}", path.display()).into())
}

/// `text` with its `[hints]` table replaced by one holding `hints`.
pub fn with_hints(text: &str, hints: &BTreeSet<Hint>) -> String {
    let mut out = String::new();
    let mut inside = false;
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with('[') {
            inside = t == "[hints]";
        }
        if !inside {
            out.push_str(line);
            out.push('\n');
        }
    }
    while out.ends_with("\n\n") {
        out.pop();
    }
    if hints.is_empty() {
        return out;
    }
    out.push_str(
        "\n[hints]\n# Code that relift run hot-loaded or relift report added; relift build lifts it as roots.\ncode = [\n",
    );
    for h in hints {
        out.push_str(&format!("    \"{}\",\n", h.text()));
    }
    out.push_str("]\n");
    out
}

/// Adds `new` to the hints of the game.toml at `path`; returns the ones
/// that were not there before.
///
/// # Errors
///
/// A file that cannot be read, parsed or written.
pub fn add(path: &Path, new: &[Hint]) -> Result<Vec<Hint>, BoxError> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut hints = parse(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    let added: Vec<Hint> = new
        .iter()
        .filter(|h| hints.insert((*h).clone()))
        .cloned()
        .collect();
    if !added.is_empty() {
        relift_cache::write_if_changed(path, with_hints(&text, &hints).as_bytes())?;
    }
    Ok(added)
}

#[cfg(test)]
mod tests {
    use super::*;

    const GAME: &str = "# game.toml, written by relift import.\n\n[game]\nguest = \"switch\"\n\n[[game.module]]\nname = \"main\"\n";

    fn h(m: &str, o: u64) -> Hint {
        Hint {
            module: m.into(),
            offset: o,
        }
    }

    #[test]
    fn a_hint_round_trips_through_its_text() {
        let x = h("rtld-x", 0x170c0);
        assert_eq!(x.text(), "rtld-x+0x170c0");
        assert_eq!(Hint::parse(&x.text()).unwrap(), x);
        assert!(Hint::parse("main+170c0").is_err());
        assert!(Hint::parse("+0x10").is_err());
        assert!(Hint::parse("main").is_err());
    }

    #[test]
    fn the_hints_table_is_replaced_and_the_rest_kept() {
        let one: BTreeSet<Hint> = [h("main", 0x40)].into();
        let t = with_hints(GAME, &one);
        assert!(t.starts_with(GAME), "{t}");
        assert_eq!(parse(&t).unwrap(), one);
        let two: BTreeSet<Hint> = [h("main", 0x40), h("sdk", 0x8)].into();
        let t2 = with_hints(&t, &two);
        assert_eq!(t2.matches("[hints]").count(), 1, "{t2}");
        assert_eq!(parse(&t2).unwrap(), two);
        assert_eq!(parse(GAME).unwrap(), BTreeSet::new());
        assert_eq!(with_hints(&t2, &BTreeSet::new()), GAME);
    }

    #[test]
    fn a_table_after_the_hints_is_kept() {
        let t = format!("{GAME}\n[hints]\ncode = [\"main+0x4\"]\n\n[other]\nx = 1\n");
        let more: BTreeSet<Hint> = [h("main", 0x4), h("main", 0x8)].into();
        let out = with_hints(&t, &more);
        let v: toml::Table = out.parse().unwrap();
        assert_eq!(v["other"]["x"].as_integer(), Some(1));
        assert_eq!(parse(&out).unwrap(), more);
    }
}
