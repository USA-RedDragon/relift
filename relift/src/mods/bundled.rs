//! Bundled mods: code mods the game repo declares in `game.toml`, built
//! into the binary and switched on or off at start.
//!
//! ```toml
//! [[mods.bundled]]
//! name = "fast_text"
//! label = "Fast text"
//! description = "Text appears at once."
//! default = false
//! patch = "mods/fast_text"     # a mod directory, an .ips or a .pchtxt
//! exclusive = ["slow_text"]    # never on together
//! after = ["base_fix"]         # applied after base_fix when both are on
//! ```
//!
//! Every function a bundled mod patches is lifted once per combination of
//! the mods that patch it, and calls to it go through a slot the runtime
//! points at the variant for the mods that are on (`relift/mods.h`). Two
//! mods may patch one function only when they are declared exclusive or
//! ordered. Each mod is a bool setting `mod.<name>`.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::ops::Range;
use std::path::Path;

use super::{ExefsPatch, user};
use crate::import::BoxError;

/// The most mods that may patch one function (the variants double with each).
pub const MAX_PER_FUNCTION: usize = 4;

/// One bundled mod.
#[derive(Clone, Debug)]
pub struct Bundled {
    /// The name, lowercase letters, digits and `_`.
    pub name: String,
    /// The label a launcher shows.
    pub label: String,
    /// The longer description.
    pub description: String,
    /// Whether it is on by default.
    pub default: bool,
    /// The mods it is never on together with.
    pub exclusive: Vec<String>,
    /// The mods it applies after.
    pub after: Vec<String>,
    /// Its exefs patches.
    pub patches: Vec<ExefsPatch>,
    /// SHA-256 of its files.
    pub hash: String,
}

impl Bundled {
    /// Its setting key.
    pub fn key(&self) -> String {
        format!("mod.{}", self.name)
    }
}

fn strings(t: &toml::Table, k: &str, name: &str) -> Result<Vec<String>, BoxError> {
    match t.get(k) {
        None => Ok(Vec::new()),
        Some(toml::Value::Array(a)) => a
            .iter()
            .map(|v| {
                v.as_str()
                    .map(String::from)
                    .ok_or_else(|| format!("mods.bundled {name}: {k} lists a non-string").into())
            })
            .collect(),
        Some(_) => Err(format!("mods.bundled {name}: {k} is not a list").into()),
    }
}

/// Reads the bundled mods of `game` (the parsed `game.toml`), with patch
/// paths relative to `dir`.
///
/// # Errors
///
/// A mod is malformed, names an unknown or later mod in `after`, an unknown
/// mod in `exclusive`, has an override source (not built yet), or its patch
/// does not read.
pub fn read(game: &toml::Table, dir: &Path, title_id: &str) -> Result<Vec<Bundled>, BoxError> {
    let Some(list) = game
        .get("mods")
        .and_then(|m| m.get("bundled"))
        .and_then(|b| b.as_array())
    else {
        return Ok(Vec::new());
    };
    let mut out: Vec<Bundled> = Vec::new();
    for v in list {
        let t = v.as_table().ok_or("mods.bundled holds a non-table")?;
        let s = |k: &str| t.get(k).and_then(|v| v.as_str()).map(String::from);
        let name = s("name").ok_or("a bundled mod has no name")?;
        if name.is_empty()
            || !name
                .chars()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
        {
            return Err(format!(
                "mods.bundled {name:?}: a name is lowercase letters, digits and _"
            )
            .into());
        }
        if out.iter().any(|b| b.name == name) {
            return Err(format!("mods.bundled {name}: declared twice").into());
        }
        if t.contains_key("override") {
            return Err(format!(
                "mods.bundled {name}: override sources need game repo overrides, which relift does not build yet"
            )
            .into());
        }
        let patch = s("patch").ok_or_else(|| format!("mods.bundled {name}: no patch"))?;
        let m = user::read(&dir.join(&patch), title_id)
            .map_err(|e| format!("mods.bundled {name}: {e}"))?;
        if !m.replacements.is_empty() {
            return Err(format!(
                "mods.bundled {name}: a bundled mod patches functions; it cannot replace a whole module"
            )
            .into());
        }
        let after = strings(t, "after", &name)?;
        for a in &after {
            if !out.iter().any(|b| &b.name == a) {
                return Err(format!(
                    "mods.bundled {name}: after names {a}, which is not declared before it"
                )
                .into());
            }
        }
        out.push(Bundled {
            label: s("label").unwrap_or_else(|| name.clone()),
            description: s("description").unwrap_or_default(),
            default: t
                .get("default")
                .and_then(toml::Value::as_bool)
                .unwrap_or(false),
            exclusive: strings(t, "exclusive", &name)?,
            after,
            patches: m.patches,
            hash: m.hash,
            name,
        });
    }
    for b in &out {
        for x in &b.exclusive {
            if !out.iter().any(|o| &o.name == x) || x == &b.name {
                return Err(
                    format!("mods.bundled {}: exclusive names unknown mod {x}", b.name).into(),
                );
            }
        }
    }
    Ok(out)
}

/// Whether mods `i` and `j` are declared exclusive.
pub fn exclusive(list: &[Bundled], i: usize, j: usize) -> bool {
    list[i].exclusive.contains(&list[j].name) || list[j].exclusive.contains(&list[i].name)
}

/// Whether mods `i` and `j` are declared ordered, in either direction.
pub fn ordered(list: &[Bundled], i: usize, j: usize) -> bool {
    list[i].after.contains(&list[j].name) || list[j].after.contains(&list[i].name)
}

/// The exclusive pairs, by index.
pub fn exclusive_pairs(list: &[Bundled]) -> Vec<(usize, usize)> {
    let mut out = BTreeSet::new();
    for i in 0..list.len() {
        for j in i + 1..list.len() {
            if exclusive(list, i, j) {
                out.insert((i, j));
            }
        }
    }
    out.into_iter().collect()
}

/// One mod's writes into one module image.
#[derive(Clone, Debug)]
pub struct ModImage {
    /// The image ranges written.
    pub ranges: Vec<Range<u64>>,
    /// The image with this mod alone applied.
    pub image: Vec<u8>,
}

/// Applies each bundled mod alone to `image` (a module with build id `id`).
///
/// # Errors
///
/// A patch runs past the module.
pub fn per_mod(
    list: &[Bundled],
    id: &[u8; 32],
    image: &[u8],
) -> Result<Vec<Option<ModImage>>, BoxError> {
    list.iter()
        .map(|b| {
            let mut img = image.to_vec();
            let mut ranges = Vec::new();
            for p in b.patches.iter().filter(|p| p.matches(id)) {
                ranges.extend(
                    p.apply(&mut img)
                        .map_err(|e| format!("mods.bundled {}: {e}", b.name))?,
                );
            }
            Ok((!ranges.is_empty()).then(|| ModImage {
                ranges: relift_patch::merge(ranges),
                image: img,
            }))
        })
        .collect()
}

/// `image` with the mods `on` applied in order, each over its own ranges.
pub fn combine(image: &[u8], mods: &[Option<ModImage>], on: &[usize]) -> Vec<u8> {
    let mut out = image.to_vec();
    for &k in on {
        if let Some(m) = &mods[k] {
            for r in &m.ranges {
                let (s, e) = (r.start as usize, r.end as usize);
                out[s..e].copy_from_slice(&m.image[s..e]);
            }
        }
    }
    out
}

/// A function some bundled mods patch, and its variants.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Swap {
    /// The guest entry.
    pub entry: u64,
    /// The mods that patch it, in declared order.
    pub mods: Vec<usize>,
}

impl Swap {
    /// The mods on in variant `mask` (bit k for `mods[k]`), or `None` when
    /// two of them are exclusive.
    pub fn on(&self, list: &[Bundled], mask: usize) -> Option<Vec<usize>> {
        let on: Vec<usize> = self
            .mods
            .iter()
            .enumerate()
            .filter(|(k, _)| mask & (1 << k) != 0)
            .map(|(_, &m)| m)
            .collect();
        for (x, &i) in on.iter().enumerate() {
            for &j in &on[x + 1..] {
                if exclusive(list, i, j) {
                    return None;
                }
            }
        }
        Some(on)
    }
}

/// The functions bundled mods patch, from each function's entry to the
/// mods (in declared order) whose writes touch it.
///
/// # Errors
///
/// Two mods patch one function and are neither exclusive nor ordered, or
/// more than [`MAX_PER_FUNCTION`] mods patch one function.
pub fn swaps(
    list: &[Bundled],
    touched: &BTreeMap<u64, Vec<usize>>,
    label: &dyn Fn(u64) -> String,
) -> Result<Vec<Swap>, BoxError> {
    let mut out = Vec::new();
    for (&e, mods) in touched {
        if mods.len() > MAX_PER_FUNCTION {
            return Err(format!(
                "{} is patched by {} bundled mods; at most {MAX_PER_FUNCTION} may patch one function",
                label(e),
                mods.len()
            )
            .into());
        }
        for (x, &i) in mods.iter().enumerate() {
            for &j in &mods[x + 1..] {
                if !exclusive(list, i, j) && !ordered(list, i, j) {
                    return Err(format!(
                        "the bundled mods {} and {} both patch {}; declare them exclusive or give one an after = [...] on the other",
                        list[i].name,
                        list[j].name,
                        label(e)
                    )
                    .into());
                }
            }
        }
        out.push(Swap {
            entry: e,
            mods: mods.clone(),
        });
    }
    Ok(out)
}

/// One swapped function as `mods.c` writes it.
#[derive(Clone, Debug)]
pub struct SwapC {
    /// The guest entry.
    pub entry: u64,
    /// The switch function the main unit calls and registers.
    pub switch: String,
    /// The mods that patch it.
    pub mods: Vec<usize>,
    /// The variants' C functions, by mask; `None` for an exclusive mix.
    pub variants: Vec<Option<String>>,
}

/// The C symbol of the bundled mods table.
pub const SYMBOL: &str = "rl_mods_bundled";

fn c_bytes(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:#04x},")).collect()
}

/// `mods.c`: the variants' declarations, the slots and switch functions, the
/// data writes per mod (guest address and bytes) and the `rl_mods` table.
pub fn mods_c(
    headers: &[&str],
    list: &[Bundled],
    swaps: &[SwapC],
    data: &[Vec<(u64, Vec<u8>)>],
) -> String {
    let mut t = String::new();
    for h in headers {
        let _ = writeln!(t, "#include <{h}>");
    }
    t.push_str("#include <relift/mods.h>\n");
    for (k, s) in swaps.iter().enumerate() {
        for v in s.variants.iter().flatten() {
            let _ = writeln!(t, "RL_LIFTED({v});");
        }
        let _ = writeln!(
            t,
            "static rl_fn slot{k} = {};\nRL_LIFTED({});\nRL_LIFTED({}) {{ RL_MUSTTAIL return slot{k}(c, mem); }}",
            s.variants[0].as_deref().unwrap_or("0"),
            s.switch,
            s.switch
        );
        let _ = writeln!(
            t,
            "static const uint16_t fn_mods{k}[] = {{ {} }};\nstatic const rl_fn variants{k}[] = {{ {} }};",
            s.mods.iter().map(|m| format!("{m}u,")).collect::<String>(),
            s.variants
                .iter()
                .map(|v| format!("{},", v.as_deref().unwrap_or("0")))
                .collect::<String>()
        );
    }
    for (m, d) in data.iter().enumerate() {
        for (j, (_, b)) in d.iter().enumerate() {
            let _ = writeln!(
                t,
                "static const uint8_t bytes{m}_{j}[] = {{ {} }};",
                c_bytes(b)
            );
        }
        let _ = writeln!(
            t,
            "static const rl_mod_data data{m}[] = {{ {} {{ 0, 0, 0 }} }};",
            d.iter()
                .enumerate()
                .map(|(j, (a, b))| format!("{{ UINT64_C({a:#x}), {}u, bytes{m}_{j} }},", b.len()))
                .collect::<String>()
        );
    }
    t.push_str("static const rl_mod list[] = {\n");
    for (m, b) in list.iter().enumerate() {
        let _ = writeln!(
            t,
            "    {{ {:?}, {}u, data{m} }},",
            b.key(),
            data.get(m).map_or(0, Vec::len)
        );
    }
    t.push_str("};\nstatic const rl_mod_fn fns[] = {\n");
    for (k, s) in swaps.iter().enumerate() {
        let _ = writeln!(
            t,
            "    {{ UINT64_C({:#x}), &slot{k}, {}u, fn_mods{k}, variants{k} }},",
            s.entry,
            s.mods.len()
        );
    }
    if swaps.is_empty() {
        t.push_str("    { 0, 0, 0, 0, 0 },\n");
    }
    let pairs = exclusive_pairs(list);
    let _ = writeln!(
        t,
        "}};\nstatic const uint16_t exclusive[] = {{ {} 0 }};\nconst rl_mods {SYMBOL};\nconst rl_mods {SYMBOL} = {{ {}u, list, {}u, fns, {}u, exclusive }};",
        pairs
            .iter()
            .map(|(i, j)| format!("{i}u, {j}u,"))
            .collect::<String>(),
        list.len(),
        swaps.len(),
        pairs.len()
    );
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    fn b(name: &str, exclusive: &[&str], after: &[&str]) -> Bundled {
        Bundled {
            name: name.into(),
            label: name.into(),
            description: String::new(),
            default: false,
            exclusive: exclusive.iter().map(|s| s.to_string()).collect(),
            after: after.iter().map(|s| s.to_string()).collect(),
            patches: Vec::new(),
            hash: String::new(),
        }
    }

    #[test]
    fn mods_on_one_function_need_exclusive_or_after() {
        let label = |e: u64| format!("f{e:x}");
        let t = BTreeMap::from([(0x10, vec![0, 1])]);
        let list = [b("a", &[], &[]), b("b", &[], &[])];
        let e = swaps(&list, &t, &label).expect_err("refused").to_string();
        assert!(e.contains("a and b both patch f10"), "{e}");
        let list = [b("a", &[], &[]), b("b", &[], &["a"])];
        assert_eq!(swaps(&list, &t, &label).expect("ordered").len(), 1);
        let list = [b("a", &["b"], &[]), b("b", &[], &[])];
        let s = swaps(&list, &t, &label).expect("exclusive");
        assert_eq!(s[0].on(&list, 3), None);
        assert_eq!(s[0].on(&list, 2), Some(vec![1]));
    }

    #[test]
    fn mods_c_lists_slots_variants_and_data() {
        let list = [b("a", &["b"], &[]), b("b", &[], &[])];
        let c = mods_c(
            &["relift/a64/abi.h"],
            &list,
            &[SwapC {
                entry: 0x10,
                switch: "sw_10".into(),
                mods: vec![0, 1],
                variants: vec![
                    Some("f_m0".into()),
                    Some("f_m1".into()),
                    Some("f_m2".into()),
                    None,
                ],
            }],
            &[vec![(0x8000_0010, vec![1, 2])], Vec::new()],
        );
        assert!(c.contains("static rl_fn slot0 = f_m0;"), "{c}");
        assert!(
            c.contains("RL_LIFTED(sw_10) { RL_MUSTTAIL return slot0(c, mem); }"),
            "{c}"
        );
        assert!(c.contains("variants0[] = { f_m0,f_m1,f_m2,0, }"), "{c}");
        assert!(c.contains("{ UINT64_C(0x80000010), 2u, bytes0_0 },"), "{c}");
        assert!(c.contains("{ \"mod.a\", 1u, data0 },"), "{c}");
        assert!(c.contains("= { 2u, list, 1u, fns, 1u, exclusive };"), "{c}");
    }

    #[test]
    fn combine_applies_each_mod_over_its_ranges_in_order() {
        let base = vec![0u8; 4];
        let m = |at: usize, v: u8| {
            let mut image = base.clone();
            image[at] = v;
            Some(ModImage {
                ranges: std::iter::once(at as u64..at as u64 + 1).collect(),
                image,
            })
        };
        let mods = [m(1, 5), m(1, 6), m(3, 7)];
        assert_eq!(combine(&base, &mods, &[0, 2]), [0, 5, 0, 7]);
        assert_eq!(combine(&base, &mods, &[0, 1]), [0, 6, 0, 0]);
    }
}
