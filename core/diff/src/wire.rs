use std::fmt::Write;

/// One named register value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reg {
    /// The register's name, without spaces or `=`.
    pub name: String,
    /// The value, zero-extended.
    pub value: u128,
}

/// The state a driver sends: where to start, where to stop and the
/// registers at the start.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct State {
    /// The guest address of the function.
    pub pc: u64,
    /// The address the function returns to.
    pub stop: u64,
    /// The registers, in the order the report lists them.
    pub regs: Vec<Reg>,
}

impl State {
    /// The value of the register `name`.
    pub fn get(&self, name: &str) -> Option<u128> {
        self.regs.iter().find(|r| r.name == name).map(|r| r.value)
    }

    /// The line a driver sends for this state.
    pub fn line(&self) -> String {
        let mut s = format!("STATE pc={:x} stop={:x}", self.pc, self.stop);
        push_regs(&mut s, &self.regs);
        s
    }
}

pub(crate) fn push_regs(s: &mut String, regs: &[Reg]) {
    for r in regs {
        let _ = write!(s, " {}={:x}", r.name, r.value);
    }
}

fn pair(field: &str) -> Result<(&str, u128), String> {
    let (name, value) = field
        .split_once('=')
        .ok_or_else(|| format!("`{field}` is not name=hex"))?;
    let value = u128::from_str_radix(value, 16).map_err(|e| format!("`{field}`: {e}"))?;
    Ok((name, value))
}

/// Parses a `STATE` line.
///
/// # Errors
///
/// A line that is not `STATE pc=<hex> stop=<hex> name=<hex>...`.
pub fn parse_state(line: &str) -> Result<State, String> {
    let mut f = line.split_whitespace();
    if f.next() != Some("STATE") {
        return Err(format!("expected STATE, got `{line}`"));
    }
    let mut pc = None;
    let mut stop = None;
    let mut regs = Vec::new();
    for field in f {
        let (name, value) = pair(field)?;
        match name {
            "pc" => pc = Some(u64::try_from(value).map_err(|e| e.to_string())?),
            "stop" => stop = Some(u64::try_from(value).map_err(|e| e.to_string())?),
            _ => regs.push(Reg {
                name: name.to_string(),
                value,
            }),
        }
    }
    Ok(State {
        pc: pc.ok_or("STATE has no pc")?,
        stop: stop.ok_or("STATE has no stop")?,
        regs,
    })
}

/// Parses an `AFTER` line: the registers the lifted code left.
///
/// # Errors
///
/// A line that is not `AFTER name=<hex>...`.
pub fn parse_after(line: &str) -> Result<Vec<Reg>, String> {
    let mut f = line.split_whitespace();
    if f.next() != Some("AFTER") {
        return Err(format!("expected AFTER, got `{line}`"));
    }
    f.map(|field| {
        let (name, value) = pair(field)?;
        Ok(Reg {
            name: name.to_string(),
            value,
        })
    })
    .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_state_round_trips() {
        let s = State {
            pc: 0x8004000,
            stop: 0x8004100,
            regs: vec![
                Reg {
                    name: "x0".into(),
                    value: 5,
                },
                Reg {
                    name: "v0".into(),
                    value: u128::MAX,
                },
            ],
        };
        assert_eq!(parse_state(&s.line()).unwrap(), s);
        assert_eq!(s.get("x0"), Some(5));
        assert_eq!(s.get("x1"), None);
    }

    #[test]
    fn malformed_lines_are_refused() {
        assert!(parse_state("STATE pc=1").is_err());
        assert!(parse_state("STATE pc=zz stop=1").is_err());
        assert!(parse_state("AFTER x0=1").is_err());
        assert!(parse_after("AFTER x0").is_err());
        assert_eq!(parse_after("AFTER x0=a").unwrap()[0].value, 10);
    }
}
