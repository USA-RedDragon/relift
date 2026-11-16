//! The ares oracle: `n64-oracle step` run as a separate process, one JSON
//! state in and one out per case (see `check/oracle/ares/README.md`).

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use super::cases::{Case, DATA_PA, state_json};
use super::json::{J, jparse};
use super::run::Out;

/// The oracle binary: `--bin`, `RELIFT_ARES_ORACLE`, else the build script's
/// default location.
pub fn find(bin: Option<PathBuf>) -> Result<PathBuf, String> {
    let path = bin
        .or_else(|| std::env::var_os("RELIFT_ARES_ORACLE").map(PathBuf::from))
        .or_else(|| {
            std::env::var_os("HOME")
                .map(|h| Path::new(&h).join(".cache/relift/n64-ares/bin/n64-oracle"))
        })
        .ok_or("no oracle binary: set RELIFT_ARES_ORACLE")?;
    if path.is_file() {
        Ok(path)
    } else {
        Err(format!(
            "{} is missing: build it with check/oracle/ares/build.sh",
            path.display()
        ))
    }
}

/// The cases the oracle runs in one process; its command line holds one path
/// per case.
const BATCH: usize = 400;

/// Runs `cases` on the oracle; a case it gave no answer for is `None`.
pub fn run(
    bin: &Path,
    rom: Option<&Path>,
    work: &Path,
    cases: &[Case],
) -> Result<Vec<Option<Out>>, String> {
    let (inp, outp) = (work.join("in"), work.join("out"));
    for d in [&inp, &outp] {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let mut files = Vec::new();
    for c in cases {
        let p = inp.join(format!("{}.json", c.name));
        std::fs::write(&p, state_json(c)).map_err(|e| format!("{}: {e}", p.display()))?;
        files.push(p);
    }
    let answered = |c: &Case| outp.join(format!("{}.out.json", c.name)).exists();
    let step = |files: &[PathBuf]| -> Result<bool, String> {
        let mut cmd = Command::new(bin);
        cmd.arg("step").args(files).arg("--out").arg(&outp);
        if let Some(r) = rom {
            cmd.arg("--rom").arg(r);
        }
        cmd.stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .map_err(|e| format!("{}: {e}", bin.display()))
    };
    let mut from = 0;
    while from < files.len() {
        let to = (from + BATCH).min(files.len());
        if step(&files[from..to])? {
            from = to;
            continue;
        }
        let mut k = from;
        while k < to && answered(&cases[k]) {
            k += 1;
        }
        if k < to {
            step(&files[k..=k])?;
            k += 1;
        }
        from = k;
    }
    cases
        .iter()
        .map(|c| {
            let p = outp.join(format!("{}.out.json", c.name));
            match std::fs::read(&p) {
                Ok(t) => Ok(Some(parse(&jparse(&t, &mut 0)))),
                Err(_) => Ok(None),
            }
        })
        .collect()
}

fn parse(j: &J) -> Out {
    let arr = |k: &str| -> [u64; 32] {
        let mut a = [0u64; 32];
        if let Some(J::A(v)) = j.get(k) {
            for (i, x) in v.iter().enumerate().take(32) {
                a[i] = x.u();
            }
        }
        a
    };
    let mut cop0 = BTreeMap::new();
    if let Some(J::O(o)) = j.get("cop0") {
        for (k, v) in o {
            cop0.insert(k.clone(), v.u());
        }
    }
    let mut data = Vec::new();
    if let Some(J::A(m)) = j.get("mem") {
        for r in m {
            if r.get("addr").map(J::u) == Some(u64::from(DATA_PA))
                && let Some(J::S(h)) = r.get("bytes")
            {
                data = (0..h.len() / 2)
                    .map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).unwrap_or(0))
                    .collect();
            }
        }
    }
    let exc = match j.get("exceptions") {
        Some(J::O(o)) if !o.is_empty() => o[0].0.parse().ok(),
        _ => None,
    };
    let delay = exc.is_some() && cop0.get("cause").is_some_and(|c| c >> 31 & 1 == 1);
    Out {
        gpr: arr("gpr"),
        hi: j.get("hi").map_or(0, J::u),
        lo: j.get("lo").map_or(0, J::u),
        fpr: arr("fpr"),
        fcr31: j.get("fcr31").map_or(0, J::u) as u32,
        cop0,
        data,
        pc: j.get("pc").map_or(0, J::u),
        exc,
        delay,
        slow: false,
    }
}
