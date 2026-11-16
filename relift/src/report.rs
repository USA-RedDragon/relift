//! `relift report`: reads a stop report a shipped build wrote (missed code,
//! an instruction it could not run, a landing pad it could not enter) and,
//! with the project's build directory, prints what a developer needs to fix
//! discovery: the transfer site's lifted function and its C at the site, the
//! guest instructions around the site and at the target, and what the build
//! knew about the target. Then it adds the target as a root for the next
//! build, as `relift run` does for code it hot-loads.
//!
//! The report format (the runtime's `rl_report_stop`, docs/abi.md section 11)
//! is plain text: a first line `relift stop report <version>`, `key: value`
//! lines, an indented register block and backtraces, and `end of report`.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use crate::hints::Hint;
use crate::import::BoxError;
use crate::settings::Settings;

/// The report format version this relift reads.
const FORMAT: &str = "relift stop report 1";

/// A parsed stop report: its `key: value` lines in order and its blocks.
#[derive(Debug, Default)]
pub struct Report {
    /// The `key: value` lines, in order.
    pub fields: Vec<(String, String)>,
    /// The register lines.
    pub registers: Vec<String>,
    /// The guest backtrace lines.
    pub guest_backtrace: Vec<String>,
}

/// A guest address the report names, with its module and offset when it
/// gives them.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Place {
    /// The guest address.
    pub addr: u64,
    /// The module and the offset in it.
    pub module: Option<(String, u64)>,
}

fn hex(s: &str) -> Option<u64> {
    u64::from_str_radix(s.strip_prefix("0x")?, 16).ok()
}

impl Place {
    /// Parses `0x<addr>[ <module>+0x<off>]`.
    fn parse(v: &str) -> Option<Place> {
        let mut it = v.split_whitespace();
        let addr = hex(it.next()?)?;
        let module = it.next().and_then(|m| {
            let (name, off) = m.rsplit_once('+')?;
            Some((name.to_string(), hex(off)?))
        });
        Some(Place { addr, module })
    }
}

impl Report {
    /// Parses the text of a report.
    ///
    /// # Errors
    ///
    /// Text that is not a stop report of a format this relift reads.
    pub fn parse(text: &str) -> Result<Report, BoxError> {
        let mut lines = text.lines();
        let first = lines.next().unwrap_or_default().trim_end();
        if first != FORMAT {
            return Err(format!(
                "not a stop report (the first line is {first:?}, want {FORMAT:?})"
            )
            .into());
        }
        let mut r = Report::default();
        let mut block: Option<&str> = None;
        for l in lines {
            if l == "end of report" {
                return Ok(r);
            }
            if let Some(rest) = l.strip_prefix("  ") {
                match block {
                    Some("registers") => r.registers.push(rest.to_string()),
                    Some("guest") => r.guest_backtrace.push(rest.to_string()),
                    _ => {}
                }
                continue;
            }
            block = if l == "registers:" {
                Some("registers")
            } else if l.starts_with("guest backtrace:") {
                Some("guest")
            } else {
                None
            };
            if block.is_none()
                && let Some((k, v)) = l.split_once(": ")
            {
                r.fields.push((k.to_string(), v.to_string()));
            }
        }
        Err("the report is cut short (no `end of report` line)".into())
    }

    /// The first value of key.
    #[must_use]
    pub fn get(&self, key: &str) -> Option<&str> {
        self.fields
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }

    /// The target of the transfer, or the instruction of a stop at one.
    #[must_use]
    pub fn target(&self) -> Option<Place> {
        self.get("target")
            .or_else(|| self.get("at"))
            .and_then(Place::parse)
    }

    /// The transfer site, when the report knows it.
    #[must_use]
    pub fn site(&self) -> Option<Place> {
        self.get("site").and_then(Place::parse)
    }

    /// The guest platform (`switch`, `gb`, `gba`).
    #[must_use]
    pub fn guest(&self) -> Option<&str> {
        self.get("guest")
    }

    /// The instruction words of a `<label> 0x<start>: <w> <w> ...` line, as
    /// the start address and the words.
    #[must_use]
    pub fn words(&self, label: &str) -> Option<(u64, Vec<u64>)> {
        let (k, v) = self
            .fields
            .iter()
            .find(|(k, _)| k.starts_with(label) && k.len() > label.len())?;
        let start = hex(k[label.len()..].trim())?;
        let words = v
            .split_whitespace()
            .map(|w| u64::from_str_radix(w, 16).ok())
            .collect::<Option<Vec<_>>>()?;
        Some((start, words))
    }
}

/// The instruction width the report's words have.
fn word_width(words: &[u64], text: &str) -> usize {
    text.split_whitespace()
        .next()
        .map_or(4, |w| w.len() / 2)
        .max(usize::from(words.is_empty()))
}

/// The disassembly of `words` (little-endian, `width` bytes each) at
/// `start` with llvm-mc for `triple`, one line per instruction; None when
/// llvm-mc is not on `PATH` or fails.
fn llvm_mc(triple: &str, start: u64, words: &[u64], width: usize) -> Option<Vec<String>> {
    let tool = relift_build::find_tool("llvm-mc")?;
    let mut bytes = String::new();
    for w in words {
        for k in 0..width {
            let _ = write!(bytes, "0x{:02x} ", (w >> (8 * k)) & 0xff);
        }
    }
    let mut child = relift_proc::command(tool)
        .args([
            "--disassemble",
            &format!("-triple={triple}"),
            "--show-encoding",
        ])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .ok()?;
    {
        use std::io::Write as _;
        child.stdin.take()?.write_all(bytes.as_bytes()).ok()?;
    }
    let out = child.wait_with_output().ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let mut a = start;
    let mut lines = Vec::new();
    for l in text.lines() {
        let l = l.trim();
        if l.is_empty() || l.starts_with('.') || l.starts_with("//") || l.starts_with('@') {
            continue;
        }
        let n = l
            .split_once("encoding: [")
            .map_or(width, |(_, e)| e.split(',').count());
        let insn = l
            .split_once("//")
            .or_else(|| l.split_once("@ "))
            .map_or(l, |(i, _)| i)
            .trim();
        lines.push(format!("0x{a:x}: {insn}"));
        a += n as u64;
    }
    Some(lines)
}

/// The SM83 instructions in `bytes` from `start`, by the opcode table.
fn sm83_lines(start: u64, bytes: &[u8]) -> Vec<String> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < bytes.len() {
        let op = bytes[i];
        let (code, len) = if op == 0xCB && i + 1 < bytes.len() {
            let e = &relift_cpu_sm83::CB[usize::from(bytes[i + 1])];
            (
                format!("{:?} {:?}, {:?}", e.mnemonic, e.dst, e.src),
                usize::from(e.length),
            )
        } else {
            let e = &relift_cpu_sm83::BASE[usize::from(op)];
            (
                format!("{:?} {:?}, {:?}", e.mnemonic, e.dst, e.src),
                usize::from(e.length.max(1)),
            )
        };
        let end = (i + len).min(bytes.len());
        let raw: Vec<String> = bytes[i..end].iter().map(|b| format!("{b:02x}")).collect();
        out.push(format!(
            "0x{:04x}: {:<9} {code}",
            start + i as u64,
            raw.join(" ")
        ));
        i += len;
    }
    out
}

/// Whether the report's registers say the CPU was in Thumb state.
fn thumb(r: &Report) -> bool {
    r.registers
        .iter()
        .find_map(|l| l.strip_prefix("cpsr "))
        .and_then(|l| l.split_once('(').map(|(_, f)| f.contains('T')))
        .unwrap_or(false)
}

/// The state of the code at the report's `<what>` (`site` or `at`): from
/// the lifted function that holds it, whose host name says its state
/// (`gba_t_...` Thumb, `gba_a_...` ARM; [`relift_gba::program::host_name`]),
/// else the CPU state at the stop.
fn thumb_at(r: &Report, what: &str) -> bool {
    r.get(&format!("{what} function"))
        .and_then(|f| {
            if f.contains(" gba_t_") {
                Some(true)
            } else if f.contains(" gba_a_") {
                Some(false)
            } else {
                None
            }
        })
        .unwrap_or_else(|| thumb(r))
}

/// The disassembly lines of the report's `<label>` words, in Thumb state
/// when `thumb_code`.
fn disassemble(r: &Report, label: &str, at: u64, thumb_code: bool) -> Vec<String> {
    let Some((start, words)) = r.words(label) else {
        return Vec::new();
    };
    let text = r
        .fields
        .iter()
        .find(|(k, _)| k.starts_with(label))
        .map(|(_, v)| v.as_str())
        .unwrap_or_default();
    let width = word_width(&words, text);
    let lines = match r.get("cpu") {
        Some("sm83") => {
            let skip = usize::try_from(at.saturating_sub(start))
                .unwrap_or(0)
                .min(words.len());
            let bytes: Vec<u8> = words[skip..].iter().map(|w| *w as u8).collect();
            Some(sm83_lines(start + skip as u64, &bytes))
        }
        Some("arm32") => {
            let thumb_code = thumb_code || at & 1 == 1;
            llvm_mc(
                if thumb_code { "thumbv4t" } else { "armv4t" },
                start,
                &words,
                width,
            )
        }
        Some("arm64") => llvm_mc("aarch64", start, &words, width),
        _ => None,
    };
    let lines = lines.unwrap_or_else(|| {
        words
            .iter()
            .enumerate()
            .map(|(i, w)| {
                format!(
                    "0x{:x}: {w:0width$x}",
                    start + (i * width) as u64,
                    width = 2 * width
                )
            })
            .collect()
    });
    let at = if r.get("cpu") == Some("arm32") {
        at & !1
    } else {
        at
    };
    lines
        .into_iter()
        .map(|l| {
            let here = l
                .split_once(':')
                .and_then(|(a, _)| hex(a))
                .is_some_and(|a| a == at);
            format!("{} {l}", if here { "=>" } else { "  " })
        })
        .collect()
}

/// Every C file under `dir`, the ones whose path contains `prefer` first.
fn c_files(dir: &Path, prefer: &str) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(d) else {
            return;
        };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(&p, out);
            } else if p.extension().is_some_and(|x| x == "c") {
                out.push(p);
            }
        }
    }
    let mut all = Vec::new();
    walk(dir, &mut all);
    all.sort();
    let (mut a, b): (Vec<_>, Vec<_>) = all
        .into_iter()
        .partition(|p| p.to_string_lossy().contains(prefer));
    a.extend(b);
    a
}

/// The lifted C of the instruction at `addr`: the lines from its label
/// `L_<addr>: {` to the next label, at most `max`, with the file.
fn lifted_c(cdir: &Path, place: &Place, max: usize) -> Option<(PathBuf, Vec<String>)> {
    let label = format!("L_{:x}: {{", place.addr);
    let prefer = place.module.as_ref().map_or_else(String::new, |(m, off)| {
        format!("{m}/shard{:04x}", off >> 16)
    });
    for f in c_files(cdir, &prefer) {
        let Ok(text) = std::fs::read_to_string(&f) else {
            continue;
        };
        if let Some(i) = text.find(&label) {
            let mut lines = Vec::new();
            for (k, l) in text[i..].lines().enumerate() {
                if k > 0 && l.starts_with("L_") && l.ends_with(": {") || lines.len() == max {
                    break;
                }
                lines.push(l.to_string());
                if l == "}" {
                    break;
                }
            }
            return Some((f, lines));
        }
    }
    None
}

/// What the build's entry list (`entries.txt`) and its list of instructions
/// not lifted say about `addr`.
fn analysis_state(out: &Path, addr: u64) -> Vec<String> {
    let mut notes = Vec::new();
    if let Ok(t) = std::fs::read_to_string(out.join(crate::build::ENTRIES)) {
        let hit = t.lines().find(|l| {
            l.split_whitespace()
                .nth(1)
                .and_then(hex)
                .is_some_and(|a| a == addr)
        });
        notes.push(match hit {
            Some(l) => format!("the build lists 0x{addr:x} as a function entry ({l})"),
            None => format!("0x{addr:x} is not among the build's function entries"),
        });
    }
    if let Ok(t) = std::fs::read_to_string(out.join("unlifted.txt")) {
        for l in t
            .lines()
            .filter(|l| l.split('\t').nth(1).and_then(hex) == Some(addr))
        {
            notes.push(format!("not lifted: {}", l.replace('\t', " ")));
        }
    }
    notes
}

/// The lines of the build's `unresolved.txt` (the transfers analysis left
/// to the dispatcher, written by the GBA and Game Boy builds as `<location>
/// <kind> <detail>`) whose location is the instruction at `addr`.
fn unresolved_at(out: &Path, addr: u64) -> Vec<String> {
    let Ok(t) = std::fs::read_to_string(out.join("unresolved.txt")) else {
        return Vec::new();
    };
    t.lines()
        .filter(|l| {
            l.split_whitespace().next().is_some_and(|loc| {
                let hexpart = loc.rsplit(':').next().unwrap_or(loc);
                let hexpart = hexpart.split('@').next().unwrap_or(hexpart);
                u64::from_str_radix(hexpart.trim_start_matches("0x"), 16).ok() == Some(addr & !1)
            })
        })
        .map(|l| {
            format!(
                "unresolved at the site: {}",
                l.split_whitespace().collect::<Vec<_>>().join(" ")
            )
        })
        .collect()
}

/// What Switch analysis says about the transfer at `site`: the function
/// that holds it is lifted again from the build's cached outline
/// (`analysis/<module>.zst`) and its unresolved transfers are classified
/// as `relift coverage` does. Empty when the project, its import or the
/// outline is missing.
fn switch_site(settings: &Settings, out: &Path, site: &Place) -> Vec<String> {
    let Some((module, _)) = &site.module else {
        return Vec::new();
    };
    let Ok(import) = crate::build::import_dir(settings) else {
        return Vec::new();
    };
    let Ok((mods, _)) = crate::build::load(&import) else {
        return Vec::new();
    };
    let placed = relift_switch::program::place(mods);
    let Some(i) = placed.iter().position(|p| &p.name == module) else {
        return Vec::new();
    };
    let Some(layout) = std::fs::read(out.join("analysis").join(format!("{i}.zst")))
        .ok()
        .and_then(|z| zstd::decode_all(z.as_slice()).ok())
        .and_then(|b| relift_analyze::from_bytes(&b).ok())
        .map(|(_, l)| l)
    else {
        return vec![format!(
            "the build keeps no outline of {module} (analysis/{i}.zst), so the transfer is not classified"
        )];
    };
    let Some(outline) = layout.functions.values().find(|o| {
        o.runs
            .iter()
            .any(|&(first, n)| site.addr >= first && site.addr < first + 4 * u64::from(n))
    }) else {
        return vec![format!(
            "no function the build lifted holds 0x{:x}: discovery never reached the site",
            site.addr
        )];
    };
    let hints = crate::hints::read(Path::new("game.toml")).unwrap_or_default();
    let p = &placed[i];
    let m = crate::build::module_with_hints(p, &hints);
    let linkages: Vec<_> = placed.iter().map(relift_switch::program::linkage).collect();
    let exported: std::collections::BTreeSet<String> = linkages
        .iter()
        .flat_map(|l| l.exports.iter().map(|(n, _)| n.clone()))
        .collect();
    let slots: std::collections::BTreeMap<u64, String> = linkages[i]
        .slots
        .iter()
        .map(|s| (s.addr, s.name.clone()))
        .collect();
    let addrs: std::collections::BTreeSet<u64> = slots.keys().copied().collect();
    let cpu = relift_cpu_arm64::frontend::Arm64 {
        features: relift_switch::CPU_FEATURES,
    };
    let preserved = relift_guest::Frontend::preserved(&cpu);
    let at = site.addr;
    relift_analyze::each_lifted(&cpu, &m, &[outline], |_, f| {
        if let Some(t) = f.tables.get(&at) {
            return vec![format!(
                "analysis resolved the site as a jump table of {} targets; the target is none of them",
                t.len()
            )];
        }
        let Some(t) = relift_analyze::unresolved_transfers(f, &preserved)
            .into_iter()
            .find(|t| t.pc == at)
        else {
            return vec!["analysis found no unresolved transfer at the site".to_string()];
        };
        let via = if addrs.is_empty() {
            None
        } else {
            relift_analyze::slot_uses(f, &addrs, 8)
                .transfers
                .get(&at)
                .and_then(|s| slots.get(s))
                .cloned()
        };
        let reason = crate::coverage::classify(&t, via.as_deref(), &|s| exported.contains(s));
        vec![format!(
            "analysis left this computed {} unresolved: reason {}, target {}{}",
            crate::coverage::kind_name(t.kind),
            reason.map_or("loader slot (exported)", crate::coverage::Reason::name),
            crate::coverage::source_name(t.source),
            via.map_or_else(String::new, |s| format!(", through the slot of {s}"))
        )]
    })
    .into_iter()
    .flatten()
    .collect()
}

/// The learned-file line of a Game Boy target: `BBB:AAAA` for a ROM
/// location (the bank from the report's `target where` line), `ram AAAA
/// KKK` for code in RAM (the opcode key from the target's first bytes).
fn gb_learned(r: &Report, addr: u64) -> Option<String> {
    if addr >= 0x8000 {
        let (_, words) = r.words("target words")?;
        let op = *words.first()?;
        let key = if op == 0xCB {
            0x100 | *words.get(1)?
        } else {
            op
        };
        return Some(format!("ram {addr:04x} {key:03x}"));
    }
    let w = r.get("target where")?;
    let bank: u64 = w
        .strip_prefix("ROM bank ")?
        .split(|c: char| !c.is_ascii_digit())
        .next()?
        .parse()
        .ok()?;
    Some(format!("{bank:03x}:{addr:04x}"))
}

/// Adds `line` to the learned file `path` of the Game Boy project, which
/// the next build lifts from; says what it did.
fn learn(path: &Path, line: &str) -> Result<String, BoxError> {
    let old = std::fs::read_to_string(path).unwrap_or_default();
    if old.lines().any(|l| l.trim() == line) {
        return Ok(format!("{line} is already in {}", path.display()));
    }
    let mut text = old;
    if !text.is_empty() && !text.ends_with('\n') {
        text.push('\n');
    }
    text.push_str(line);
    text.push('\n');
    relift_cache::write_if_changed(path, text.as_bytes())?;
    Ok(format!(
        "added {line} to {}; the next relift build lifts it",
        path.display()
    ))
}

/// What `relift report` does after printing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HintMode {
    /// Add the target as a root for the next build.
    Add,
    /// Print only.
    None,
}

/// The text `relift report` prints for report `r` with the build directory
/// `out` (None: report only).
#[must_use]
pub fn explain(r: &Report, out: Option<&Path>) -> String {
    let mut t = String::new();
    let g = |k: &str| r.get(k).unwrap_or("-");
    let _ = writeln!(t, "{}: {}", g("kind"), g("summary"));
    for k in [
        "relift",
        "game",
        "build",
        "guest",
        "mods",
        "code mods",
        "image",
    ] {
        if let Some(v) = r.get(k) {
            let _ = writeln!(t, "  {k}: {v}");
        }
    }
    let _ = writeln!(t);
    let transfer = r.get("transfer").unwrap_or("none");
    let _ = writeln!(t, "transfer: {transfer}");
    for k in [
        "target",
        "at",
        "at function",
        "target code",
        "target function",
        "target where",
        "site",
        "site function",
        "caller function",
    ] {
        if let Some(v) = r.get(k) {
            let _ = writeln!(t, "  {k}: {v}");
        }
    }
    if let Some(site) = r.site().filter(|_| transfer != "none") {
        let d = disassemble(r, "site words", site.addr, thumb_at(r, "site"));
        if !d.is_empty() {
            let _ = writeln!(t, "\nguest code at the site:");
            for l in d {
                let _ = writeln!(t, "  {l}");
            }
        }
    } else if transfer == "none"
        && let Some(at) = r.target()
    {
        let d = disassemble(r, "at words", at.addr, thumb_at(r, "at"));
        if !d.is_empty() {
            let _ = writeln!(t, "\nguest code at the stop:");
            for l in d {
                let _ = writeln!(t, "  {l}");
            }
        }
    }
    if let Some(target) = r.target().filter(|_| transfer != "none") {
        let d = disassemble(r, "target words", target.addr, thumb(r));
        if !d.is_empty() {
            let _ = writeln!(t, "\nguest code at the target:");
            for l in d {
                let _ = writeln!(t, "  {l}");
            }
        }
    }
    if !r.guest_backtrace.is_empty() {
        let _ = writeln!(t, "\nguest backtrace:");
        for l in &r.guest_backtrace {
            let _ = writeln!(t, "  {l}");
        }
    }
    if let Some(out) = out {
        let cdir = out.join("c");
        if let Some(site) = r.site() {
            match lifted_c(&cdir, &site, 40) {
                Some((f, lines)) => {
                    let _ = writeln!(
                        t,
                        "\nlifted C at the site ({}):",
                        f.strip_prefix(out).unwrap_or(&f).display()
                    );
                    for l in lines {
                        let _ = writeln!(t, "  {l}");
                    }
                }
                None => {
                    let _ = writeln!(
                        t,
                        "\nthe build in {} has no lifted code at the site",
                        out.display()
                    );
                }
            }
        }
        if let Some(site) = r.site() {
            let notes = unresolved_at(out, site.addr);
            if !notes.is_empty() {
                let _ = writeln!(t, "\nwhat the build knew about the site:");
                for n in notes {
                    let _ = writeln!(t, "  {n}");
                }
            }
        }
        if let Some(target) = r.target() {
            let notes = analysis_state(out, target.addr);
            if !notes.is_empty() {
                let _ = writeln!(t, "\nwhat the build knew about the target:");
                for n in notes {
                    let _ = writeln!(t, "  {n}");
                }
            }
        }
    }
    t
}

/// Adds the report's target as a root for the next build: a hint in
/// `game.toml` (Switch, and GBA for the cartridge ROM), a line of the
/// learned file `learned` (Game Boy). Returns what it did, for the user.
///
/// # Errors
///
/// A game.toml or learned file that cannot be read or written.
pub fn add_hint(
    r: &Report,
    project: &Path,
    out: Option<&Path>,
    learned: Option<&Path>,
) -> Result<String, BoxError> {
    let Some(target) = r.target() else {
        return Ok("the report names no target; nothing added".into());
    };
    if r.get("kind") != Some("missed code") {
        return Ok(format!(
            "the stop is {}, not missed code; no root added",
            r.get("kind").unwrap_or("?")
        ));
    }
    let game = std::fs::read_to_string(project.join("game.toml"))?
        .parse::<toml::Table>()
        .map_err(|e| format!("game.toml: {e}"))?;
    if let Some(why) = foreign(r, &game) {
        return Ok(format!(
            "the report is from another game than this project ({why}); no root added"
        ));
    }
    let note = out
        .and_then(build_of)
        .filter(|b| r.get("build").is_some_and(|rb| rb != b))
        .map_or_else(String::new, |b| {
            format!(
                " (the report is from build {}, the project's build is now {b})",
                r.get("build").unwrap_or("?")
            )
        });
    let added = match r.guest() {
        Some("switch") => {
            if !r
                .get("target where")
                .is_some_and(|w| w.ends_with(" text segment"))
            {
                return Ok(format!(
                    "0x{:x} is not in a module's text segment ({}); no hint added",
                    target.addr,
                    r.get("target where").unwrap_or("unknown place")
                ));
            }
            let Some((module, offset)) = target.module else {
                return Ok(format!(
                    "0x{:x} lies in no module; no hint added",
                    target.addr
                ));
            };
            hint(project, Hint { module, offset })?
        }
        Some("gba") => {
            let Some((module, offset)) = target.module.filter(|(m, _)| m == crate::gba::ROM_MODULE)
            else {
                return Ok(format!(
                    "0x{:x} is not in the cartridge ROM; code in RAM is lifted from its copy, so no hint is added",
                    target.addr
                ));
            };
            hint(project, Hint { module, offset })?
        }
        Some("gb") => {
            let Some(line) = gb_learned(r, target.addr) else {
                return Ok(format!(
                    "the report does not say what 0x{:x} holds; no root added",
                    target.addr
                ));
            };
            let Some(path) = learned else {
                return Ok("the project has no learned file; no root added".into());
            };
            learn(path, &line)?
        }
        other => {
            return Ok(format!(
                "the {} build reads no root hints yet; lift 0x{:x} by hand",
                other.unwrap_or("?"),
                target.addr
            ));
        }
    };
    Ok(added + &note)
}

/// Adds `h` to the hints of the project's game.toml; says what it did.
fn hint(project: &Path, h: Hint) -> Result<String, BoxError> {
    let added = crate::hints::add(&project.join("game.toml"), std::slice::from_ref(&h))?;
    Ok(if added.is_empty() {
        format!("{} is already a hint in game.toml", h.text())
    } else {
        format!(
            "added {} to the hints in game.toml; the next relift build lifts it",
            h.text()
        )
    })
}

/// Why the report `r` cannot be from the project whose game.toml is
/// `game`: another title or program id, a module the project does not have
/// or whose build id differs (Switch), or another cartridge code or version
/// (GBA). None when nothing says so.
fn foreign(r: &Report, game: &toml::Table) -> Option<String> {
    let g = game.get("game")?;
    let s = |k: &str| {
        g.get(k)
            .and_then(|v| v.as_str())
            .map(str::to_ascii_lowercase)
    };
    match r.guest() {
        Some("switch") => {
            if let (Some(want), Some(got)) = (s("title_id"), r.get("program"))
                && want != got.to_ascii_lowercase()
            {
                return Some(format!("program {got}, the project's is {want}"));
            }
            let mods: Vec<(String, String)> = g
                .get("module")
                .and_then(|m| m.as_array())
                .map(|a| {
                    a.iter()
                        .filter_map(|m| {
                            Some((
                                m.get("name")?.as_str()?.to_string(),
                                m.get("build_id")?.as_str()?.to_ascii_lowercase(),
                            ))
                        })
                        .collect()
                })
                .unwrap_or_default();
            for (k, v) in &r.fields {
                if k != "module" {
                    continue;
                }
                let name = v.split_whitespace().next().unwrap_or_default();
                let build = v
                    .rsplit(' ')
                    .next()
                    .unwrap_or_default()
                    .to_ascii_lowercase();
                match mods.iter().find(|(n, _)| n == name) {
                    None if !mods.is_empty() => {
                        return Some(format!("module {name}, which the project does not have"));
                    }
                    Some((_, want)) if build != "-" && *want != build => {
                        return Some(format!(
                            "module {name} build {build}, the project's is {want}"
                        ));
                    }
                    _ => {}
                }
            }
            None
        }
        Some("gba") => {
            let got = r.get("game")?;
            let words: Vec<&str> = got.split_whitespace().skip(1).collect();
            let at = words.iter().position(|w| *w == "version");
            let code = words.first().filter(|w| **w != "version").copied();
            let version = at.and_then(|i| words.get(i + 1)).copied();
            if let (Some(want), Some(code)) = (s("game_code"), code)
                && want != code.to_ascii_lowercase()
            {
                return Some(format!("cartridge {code}, the project's is {want}"));
            }
            let want_v = g
                .get("version")
                .map(|v| v.to_string().trim_matches('"').to_string());
            if let (Some(want), Some(v)) = (want_v, version)
                && want != v
            {
                return Some(format!("version {v}, the project's is {want}"));
            }
            None
        }
        _ => None,
    }
}

/// The `build:` digest of the program in the build directory `out`.
fn build_of(out: &Path) -> Option<String> {
    let exe = out.join(format!("game{}", std::env::consts::EXE_SUFFIX));
    let image = std::fs::read(exe).ok()?;
    let doc = relift_info::section::read(&image).ok()?;
    Some(relift_info::embed::build_digest(doc))
}

/// `relift report <file>`.
///
/// # Errors
///
/// A file that cannot be read or is not a stop report, or a hint that
/// cannot be written.
pub fn run(
    settings: &Settings,
    file: &Path,
    build_dir: Option<&Path>,
    hint: HintMode,
) -> Result<(), BoxError> {
    let text = std::fs::read_to_string(file).map_err(|e| format!("{}: {e}", file.display()))?;
    let r = Report::parse(&text).map_err(|e| format!("{}: {e}", file.display()))?;
    let project = Path::new("game.toml").is_file();
    let out = match build_dir {
        Some(d) => Some(d.to_path_buf()),
        None if project => crate::build::project_out(settings).ok().map(|(_, o)| o),
        None => None,
    };
    print!("{}", explain(&r, out.as_deref()));
    if r.guest() == Some("switch")
        && project
        && let (Some(out), Some(site)) = (out.as_deref(), r.site())
    {
        let notes = switch_site(settings, out, &site);
        if !notes.is_empty() {
            println!("\nwhat analysis says about the site:");
            for n in notes {
                println!("  {n}");
            }
        }
    }
    if hint == HintMode::Add && project {
        println!(
            "\n{}",
            add_hint(
                &r,
                Path::new("."),
                out.as_deref(),
                crate::gb::learned_file(settings).ok().as_deref(),
            )?
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "relift stop report 1\nkind: missed code\nsummary: no function at guest 0x8000040\nrelift: 0.1.0\nguest: switch\nmodule: main base 0x8000000 size 0x1000 build 00\ncpu: arm64\ntransfer: call\ntarget: 0x8000040 main+0x40\ntarget code: not lifted (in the code of main)\ntarget words 0x8000040: d503201f d65f03c0\nsite: 0x8000010 main+0x10\nsite function: 0x8000000 main+0x0 F_8000000 +0x10\nsite words 0x8000000: d503201f d503201f d503201f d503201f d63f0100\nregisters:\n  x0  0000000000000000\n  cpsr 6000001f (-ZC- --T sys)\nguest backtrace: lifted frames, innermost first; tail calls leave no frame\n  #0 0x8000000 main+0x0 F_8000000\nhost backtrace:\n  #0 0x1 ?\nend of report\n";

    #[test]
    fn a_report_parses_into_its_fields_and_blocks() {
        let r = Report::parse(SAMPLE).expect("parses");
        assert_eq!(r.get("kind"), Some("missed code"));
        assert_eq!(
            r.target(),
            Some(Place {
                addr: 0x800_0040,
                module: Some(("main".into(), 0x40))
            })
        );
        assert_eq!(r.site().map(|s| s.addr), Some(0x800_0010));
        assert_eq!(r.registers.len(), 2);
        assert_eq!(r.guest_backtrace, ["#0 0x8000000 main+0x0 F_8000000"]);
        assert_eq!(
            r.words("site words"),
            Some((
                0x800_0000,
                vec![
                    0xd503_201f,
                    0xd503_201f,
                    0xd503_201f,
                    0xd503_201f,
                    0xd63f_0100
                ]
            ))
        );
        assert!(thumb(&r));
    }

    #[test]
    fn a_cut_or_foreign_file_is_refused() {
        assert!(Report::parse("hello\n").is_err());
        assert!(Report::parse(&SAMPLE[..SAMPLE.len() - 14]).is_err());
    }

    #[test]
    fn the_explanation_marks_the_site_and_lists_the_transfer() {
        let r = Report::parse(SAMPLE).expect("parses");
        let t = explain(&r, None);
        assert!(
            t.starts_with("missed code: no function at guest 0x8000040\n"),
            "{t}"
        );
        assert!(t.contains("  site: 0x8000010 main+0x10\n"), "{t}");
        assert!(t.contains("=> 0x8000010: "), "{t}");
    }

    #[test]
    fn a_gb_target_is_learned_by_its_bank() {
        let r = Report::parse(
            "relift stop report 1\nkind: missed code\nguest: gb\ntarget: 0x5000\ntarget where: ROM bank 3 (mapped now), file offset 0xd000\nend of report\n",
        )
        .expect("parses");
        assert_eq!(gb_learned(&r, 0x5000).as_deref(), Some("003:5000"));
        let r = Report::parse(
            "relift stop report 1\nkind: missed code\nguest: gb\ntarget: 0xc00f\ntarget words 0xc00f: cb 37 00\nend of report\n",
        )
        .expect("parses");
        assert_eq!(gb_learned(&r, 0xc00f).as_deref(), Some("ram c00f 137"));
    }

    #[test]
    fn a_report_of_another_game_is_told_apart() {
        let game: toml::Table = "[game]\nguest = \"switch\"\ntitle_id = \"0100000000001234\"\n\n[[game.module]]\nname = \"main\"\nbuild_id = \"AB00\"\n"
            .parse()
            .expect("toml");
        let r = |extra: &str| {
            Report::parse(&format!(
                "relift stop report 1\nkind: missed code\nguest: switch\n{extra}end of report\n"
            ))
            .expect("parses")
        };
        assert_eq!(
            foreign(
                &r("program: 0100000000001234\nmodule: main base 0x8000000 size 0x10 build ab00\n"),
                &game
            ),
            None
        );
        assert!(
            foreign(&r("program: 0100000000009999\n"), &game)
                .is_some_and(|w| w.contains("program"))
        );
        assert!(
            foreign(
                &r("module: main base 0x8000000 size 0x10 build cd00\n"),
                &game
            )
            .is_some_and(|w| w.contains("build cd00"))
        );
        assert!(
            foreign(
                &r("module: sdk base 0x9000000 size 0x10 build cd00\n"),
                &game
            )
            .is_some_and(|w| w.contains("sdk"))
        );
        let gba: toml::Table = "[game]\nguest = \"gba\"\ngame_code = \"ASYN\"\nversion = \"0\"\n"
            .parse()
            .expect("toml");
        let g = |line: &str| {
            Report::parse(&format!(
                "relift stop report 1\nguest: gba\ngame: {line}\nend of report\n"
            ))
            .expect("parses")
        };
        assert_eq!(foreign(&g("gba ASYN version 0"), &gba), None);
        assert!(foreign(&g("gba BPEE version 0"), &gba).is_some());
        assert!(foreign(&g("gba ASYN version 1"), &gba).is_some());
    }

    #[test]
    fn a_switch_hint_needs_a_text_segment() {
        let dir = std::env::temp_dir().join(format!("relift-report-hint-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("dir");
        relift_cache::write_if_changed(&dir.join("game.toml"), b"[game]\nguest = \"switch\"\n")
            .expect("write");
        let r = |w: &str| {
            Report::parse(&format!(
                "relift stop report 1\nkind: missed code\nguest: switch\ntarget: 0x8001000 main+0x1000\ntarget where: {w}\nend of report\n"
            ))
            .expect("parses")
        };
        let t = add_hint(&r("main data segment"), &dir, None, None).expect("runs");
        assert!(t.contains("not in a module's text segment"), "{t}");
        let t = add_hint(&r("main text segment"), &dir, None, None).expect("runs");
        assert!(t.starts_with("added main+0x1000 to the hints"), "{t}");
        std::fs::remove_dir_all(&dir).expect("cleanup");
    }

    #[test]
    fn the_site_is_read_in_the_state_of_its_function() {
        let r = |site_fn: &str| {
            Report::parse(&format!(
                "relift stop report 1\ncpu: arm32\nsite function: 0x8000100 rom+0x100 {site_fn} +0x4\nregisters:\n  cpsr 6000001f (-ZC- --T sys)\nend of report\n"
            ))
            .expect("parses")
        };
        assert!(!thumb_at(&r("gba_a_08000100"), "site"));
        assert!(thumb_at(&r("gba_t_08000100"), "site"));
        assert!(thumb_at(&r("other"), "site"));
    }

    #[test]
    fn sm83_bytes_decode_by_the_table() {
        let l = sm83_lines(0x150, &[0x00, 0xC3, 0x00, 0x40, 0xE9]);
        assert_eq!(l.len(), 3, "{l:?}");
        assert!(l[1].starts_with("0x0151: c3 00 40"), "{l:?}");
        assert!(l[2].starts_with("0x0154: e9"), "{l:?}");
    }
}
