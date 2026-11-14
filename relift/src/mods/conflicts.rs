//! Conflicts between code mods, overrides and bundled mods, found on the
//! functions of the patched modules.
//!
//! Two user mods that patch one function are reported and applied in the
//! given order (their bytes do not overlap: [`super::user::apply`] refuses
//! that). A patch to a function the runtime overrides would have no effect,
//! and a user mod that patches a bundled mod's function would make the
//! bundled mod's variants wrong; both are refused.

use std::collections::{BTreeMap, BTreeSet};
use std::ops::Range;

use relift_analyze::{Analysis, Function, Insn};

/// The entries of the functions of `a` that hold a byte of `ranges`
/// (guest addresses).
pub fn touched(a: &Analysis, granule: u64, ranges: &[Range<u64>]) -> BTreeSet<u64> {
    a.functions
        .values()
        .filter(|f| {
            f.insts.iter().any(|(&pc, i)| {
                let len = match i {
                    Insn::Lifted(b) => u64::from(b.len).max(1),
                    Insn::Failed(_) => granule,
                };
                ranges.iter().any(|r| r.start < pc + len && pc < r.end)
            })
        })
        .map(|f| f.entry)
        .collect()
}

/// A function's name for messages.
pub fn label(module: &str, f: Option<&Function>, entry: u64) -> String {
    match f.and_then(|f| f.name.as_deref()) {
        Some(n) => format!("{module} {n} ({entry:#x})"),
        None => format!("{module} function {entry:#x}"),
    }
}

/// What one mod changed in a module, for [`check`].
#[derive(Clone, Debug)]
pub struct ModWrites {
    /// The mod's name.
    pub name: String,
    /// The guest ranges it wrote.
    pub ranges: Vec<Range<u64>>,
    /// Whether it replaced the whole module.
    pub replaced: bool,
}

/// Checks the user mods of one module against its overrides (entry to host
/// function) and its bundled mods' functions (entry to mod names). Returns
/// the notes to report.
///
/// # Errors
///
/// A user mod patches an overridden function or a bundled mod's function.
pub fn check(
    module: &str,
    a: &Analysis,
    granule: u64,
    user: &[ModWrites],
    overrides: &BTreeMap<u64, String>,
    bundled: &BTreeMap<u64, Vec<String>>,
) -> Result<Vec<String>, String> {
    let mut by_fn: BTreeMap<u64, Vec<&str>> = BTreeMap::new();
    for m in user {
        for e in touched(a, granule, &m.ranges) {
            let name = || label(module, a.functions.get(&e), e);
            if let Some(b) = bundled.get(&e) {
                return Err(format!(
                    "{} patches {}, which the bundled mod {} also patches; a user mod cannot change a bundled mod's function (turn the bundled mod off in game.toml or drop the user mod)",
                    m.name,
                    name(),
                    b.join(" and ")
                ));
            }
            if m.replaced {
                continue;
            }
            if let Some(h) = overrides.get(&e) {
                return Err(format!(
                    "{} patches {}, which the runtime replaces with the override {h}; the patch would have no effect, so the build is refused",
                    m.name,
                    name()
                ));
            }
            by_fn.entry(e).or_default().push(&m.name);
        }
    }
    Ok(by_fn
        .iter()
        .filter(|(_, v)| v.len() > 1)
        .map(|(e, v)| {
            format!(
                "{} is patched by {}, applied in that order",
                label(module, a.functions.get(e), *e),
                v.join(", then ")
            )
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_guest::toy::{Toy, branch, word};
    use relift_guest::{Module, Root, RootKind, Section, SectionKind};

    fn analysis() -> Analysis {
        let code = [
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x04, 1, 1),
            word(0x03, 0, 0),
        ];
        let image: Vec<u8> = code.iter().flatten().copied().collect();
        let m = Module {
            name: "t".into(),
            base: 0x1000,
            sections: vec![Section {
                addr: 0x1000,
                size: image.len() as u64,
                kind: SectionKind::Code,
            }],
            image,
            roots: vec![Root {
                addr: 0x1000,
                kind: RootKind::Entry,
                name: None,
            }],
            pads: Vec::new(),
        };
        relift_analyze::discover(&Toy, &m)
    }

    fn w(name: &str, at: u64) -> ModWrites {
        ModWrites {
            name: name.into(),
            ranges: std::iter::once(at..at + 1).collect(),
            replaced: false,
        }
    }

    #[test]
    fn two_user_mods_on_one_function_are_reported() {
        let a = analysis();
        assert_eq!(a.functions.len(), 2);
        let notes = check(
            "main",
            &a,
            4,
            &[w("a", 0x1008), w("b", 0x100c)],
            &BTreeMap::new(),
            &BTreeMap::new(),
        )
        .expect("allowed");
        assert_eq!(notes.len(), 1);
        assert!(notes[0].contains("patched by a, then b"), "{}", notes[0]);
    }

    #[test]
    fn a_patch_on_an_overridden_function_is_refused() {
        let a = analysis();
        let o = BTreeMap::from([(0x1008, "rl_memcpy".to_string())]);
        let e = check("main", &a, 4, &[w("a", 0x1008)], &o, &BTreeMap::new()).expect_err("refused");
        assert!(e.contains("override rl_memcpy"), "{e}");
    }

    #[test]
    fn a_user_mod_on_a_bundled_function_is_refused() {
        let a = analysis();
        let b = BTreeMap::from([(0x1008, vec!["fps60".to_string()])]);
        let e = check("main", &a, 4, &[w("a", 0x100c)], &BTreeMap::new(), &b).expect_err("refused");
        assert!(e.contains("bundled mod fps60 also patches"), "{e}");
    }
}
