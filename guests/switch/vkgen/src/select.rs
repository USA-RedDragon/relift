//! Picks the part of the registry the configuration asks for: the core
//! versions with the features they depend on, the extensions, and the
//! closure of every type, command and enum value they need.

use std::collections::{BTreeMap, BTreeSet};

use crate::Error;
use crate::config::Config;
use crate::registry::{Kind, Registry, Value};

/// What one command of the selection is for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Audience {
    /// The guest gets a thunk for it.
    Guest,
    /// Only the runtime calls it on the host.
    Host,
}

/// The selected part of the registry.
#[derive(Debug, Default)]
pub struct Selection {
    /// Selected features and extensions, by name.
    pub units: BTreeSet<String>,
    /// Defining command name to the selected names that reach it (the definition's own name when selected, then aliases) and its audience.
    pub commands: BTreeMap<String, (Vec<String>, Audience)>,
    /// Every type the selection needs, defining names and aliases.
    pub types: BTreeSet<String>,
    /// Every enum value name the selection needs.
    pub enums: BTreeSet<String>,
    /// Guest-visible instance extensions.
    pub guest_instance_exts: Vec<String>,
    /// Guest-visible device extensions.
    pub guest_device_exts: Vec<String>,
}

/// Evaluates a `depends` expression (`A+B`, `A,B`, parentheses) against the selection.
pub fn depends_hold(expr: &str, have: &dyn Fn(&str) -> bool) -> bool {
    let toks: Vec<char> = expr.chars().collect();
    let mut pos = 0;
    fn or(t: &[char], p: &mut usize, have: &dyn Fn(&str) -> bool) -> bool {
        let mut v = and(t, p, have);
        while *p < t.len() && t[*p] == ',' {
            *p += 1;
            let r = and(t, p, have);
            v = v || r;
        }
        v
    }
    fn and(t: &[char], p: &mut usize, have: &dyn Fn(&str) -> bool) -> bool {
        let mut v = atom(t, p, have);
        while *p < t.len() && t[*p] == '+' {
            *p += 1;
            let r = atom(t, p, have);
            v = v && r;
        }
        v
    }
    fn atom(t: &[char], p: &mut usize, have: &dyn Fn(&str) -> bool) -> bool {
        if *p < t.len() && t[*p] == '(' {
            *p += 1;
            let v = or(t, p, have);
            if *p < t.len() && t[*p] == ')' {
                *p += 1;
            }
            return v;
        }
        let start = *p;
        while *p < t.len() && !matches!(t[*p], '+' | ',' | '(' | ')') {
            *p += 1;
        }
        let name: String = t[start..*p].iter().collect();
        if name.contains("::") {
            return true;
        }
        have(&name)
    }
    or(&toks, &mut pos, have)
}

fn add_feature(reg: &Registry, name: &str, out: &mut BTreeSet<String>) -> Result<(), Error> {
    if out.contains(name) {
        return Ok(());
    }
    let f = reg
        .features
        .get(name)
        .ok_or_else(|| Error(format!("unknown feature {name}")))?;
    out.insert(name.to_string());
    if let Some(d) = &f.depends {
        for part in d.split(['+', ',', '(', ')']) {
            if !part.is_empty() && reg.features.contains_key(part) {
                add_feature(reg, part, out)?;
            }
        }
    }
    Ok(())
}

impl Selection {
    /// Builds the selection for cfg.
    pub fn new(reg: &Registry, cfg: &Config) -> Result<Selection, Error> {
        let mut units = BTreeSet::new();
        for f in &cfg.features {
            add_feature(reg, f, &mut units)?;
        }
        let mut sel = Selection::default();
        let all_exts: Vec<(&String, Audience)> = cfg
            .extensions
            .iter()
            .map(|e| (e, Audience::Guest))
            .chain(cfg.host_extensions.iter().map(|e| (e, Audience::Host)))
            .collect();
        for (e, _) in &all_exts {
            let u = reg
                .extensions
                .get(e.as_str())
                .ok_or_else(|| Error(format!("unknown extension {e}")))?;
            if let Some(p) = &u.platform
                && p != "vi"
            {
                return Err(Error(format!(
                    "{e} needs platform {p}: platform types are never forwarded"
                )));
            }
            units.insert(e.to_string());
        }
        let have = |n: &str| units.contains(n);
        for (e, aud) in &all_exts {
            let u = &reg.extensions[e.as_str()];
            if let Some(d) = &u.depends
                && !depends_hold(d, &have)
            {
                return Err(Error(format!(
                    "{e} depends on {d}; add what it needs to the configuration"
                )));
            }
            if *aud == Audience::Guest {
                match u.ext_type.as_deref() {
                    Some("instance") => sel.guest_instance_exts.push(e.to_string()),
                    _ => sel.guest_device_exts.push(e.to_string()),
                }
            }
        }
        let mut wanted_types = BTreeSet::new();
        let mut cmd_names: BTreeMap<String, Audience> = BTreeMap::new();
        let mut walk = |name: &str, aud: Audience| -> Result<(), Error> {
            let u = reg
                .features
                .get(name)
                .or_else(|| reg.extensions.get(name))
                .ok_or_else(|| Error(format!("unknown unit {name}")))?;
            for r in &u.requires {
                if let Some(d) = &r.depends
                    && !depends_hold(d, &have)
                {
                    continue;
                }
                wanted_types.extend(r.types.iter().cloned());
                for c in &r.commands {
                    let e = cmd_names.entry(c.clone()).or_insert(aud);
                    if aud == Audience::Guest {
                        *e = Audience::Guest;
                    }
                }
                sel.enums.extend(r.enums.iter().cloned());
            }
            Ok(())
        };
        for f in units.iter().filter(|u| reg.features.contains_key(*u)) {
            walk(f, Audience::Guest)?;
        }
        for (e, aud) in &all_exts {
            walk(e, *aud)?;
        }
        for (name, aud) in &cmd_names {
            let def = reg
                .command(name)
                .ok_or_else(|| Error(format!("unknown command {name}")))?;
            let entry = sel
                .commands
                .entry(def.name.clone())
                .or_insert((Vec::new(), *aud));
            entry.0.push(name.clone());
            if *aud == Audience::Guest {
                entry.1 = Audience::Guest;
            }
            for p in def.params.iter().chain(std::iter::once(&def.ret)) {
                wanted_types.insert(p.ty.clone());
            }
        }
        for (def, (names, _)) in sel.commands.iter_mut() {
            names.sort();
            names.dedup();
            if let Some(i) = names.iter().position(|n| n == def) {
                let n = names.remove(i);
                names.insert(0, n);
            }
        }
        let mut stack: Vec<String> = wanted_types.into_iter().collect();
        while let Some(t) = stack.pop() {
            if !sel.types.insert(t.clone()) {
                continue;
            }
            let Some(ty) = reg.types.get(&t) else {
                continue;
            };
            if let Some(r) = &ty.requires
                && reg.types.contains_key(r)
            {
                stack.push(r.clone());
            }
            match &ty.kind {
                Kind::Alias(a) => stack.push(a.clone()),
                Kind::Bitmask { over, bits } => {
                    stack.push(over.clone());
                    if let Some(b) = bits {
                        stack.push(b.clone());
                    }
                }
                Kind::Base(Some(b)) => stack.push(b.clone()),
                Kind::Struct { members, .. } => {
                    for m in members {
                        stack.push(m.ty.clone());
                        if let Some(v) = &m.values {
                            sel.enums.insert(v.clone());
                        }
                    }
                }
                Kind::FuncPtr { ret, params } => {
                    stack.push(ret.ty.clone());
                    for p in params {
                        stack.push(p.ty.clone());
                    }
                }
                _ => {}
            }
        }
        for t in &sel.types {
            if let Some(ty) = reg.types.get(t)
                && let Kind::Struct { members, .. } = &ty.kind
            {
                for m in members {
                    for d in &m.array {
                        if reg.enum_values.contains_key(d) {
                            sel.enums.insert(d.clone());
                        }
                    }
                }
            }
        }
        let mut stack: Vec<String> = sel.enums.iter().cloned().collect();
        while let Some(e) = stack.pop() {
            if let Some(Value::Alias(a)) = reg.enum_values.get(&e).map(|d| &d.value)
                && sel.enums.insert(a.clone())
            {
                stack.push(a.clone());
            }
        }
        sel.units = units;
        Ok(sel)
    }
}

#[cfg(test)]
mod tests {
    use super::depends_hold;

    #[test]
    fn depends_expressions() {
        let have = |n: &str| n == "A" || n == "B";
        assert!(depends_hold("A", &have));
        assert!(!depends_hold("C", &have));
        assert!(depends_hold("A+B", &have));
        assert!(!depends_hold("A+C", &have));
        assert!(depends_hold("C,A", &have));
        assert!(depends_hold("(C,A)+B", &have));
        assert!(!depends_hold("(C,D)+B", &have));
        assert!(depends_hold("VkX::member", &have));
    }
}
