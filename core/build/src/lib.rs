//! Toolchain discovery and the Ninja file writer that drive the C compiler.
//!
//! A [`Plan`] is a set of C files to compile, each with its own flags, and
//! one link. [`build`] writes it as `build.ninja` (through relift-cache, so
//! an unchanged plan leaves the file alone) and runs Ninja, which rebuilds
//! only what changed. Without Ninja it compiles every object whose source
//! is newer, one at a time, then links.

use std::fmt::Write as _;
use std::io;
use std::path::{Path, PathBuf};

/// One C translation unit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Compile {
    /// The source file.
    pub src: PathBuf,
    /// The object file.
    pub obj: PathBuf,
    /// The compiler flags, besides `-c`, `-o` and the dependency flags.
    pub flags: Vec<String>,
}

/// What to build.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    /// The C compiler driver (clang, or zig with `cc` as its first flag).
    pub cc: Vec<String>,
    /// The objects.
    pub compiles: Vec<Compile>,
    /// The linked executable.
    pub output: PathBuf,
    /// Link flags after the objects (libraries).
    pub link_flags: Vec<String>,
}

/// Why a build failed.
#[derive(Debug)]
pub enum Error {
    /// A file could not be written or a tool could not be started.
    Io(io::Error),
    /// A tool ran and failed.
    Failed(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Io(e) => write!(f, "{e}"),
            Error::Failed(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Error {
        Error::Io(e)
    }
}

/// Quotes `s` for a Ninja command line (POSIX shell rules).
fn sh(s: &str) -> String {
    if !s.is_empty()
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"-_./=+,:@%".contains(&b))
    {
        return s.to_string();
    }
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// Escapes a path for a Ninja build line.
fn nj(p: &Path) -> String {
    p.display()
        .to_string()
        .replace('$', "$$")
        .replace(' ', "$ ")
        .replace(':', "$:")
}

/// The text of `build.ninja` for `plan`.
pub fn ninja_file(plan: &Plan) -> String {
    let cc: Vec<String> = plan.cc.iter().map(|s| sh(s)).collect();
    let mut t = format!(
        "ninja_required_version = 1.3\ncc = {}\n\nrule cc\n  command = $cc $flags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = cc $in\n\nrule link\n  command = $cc $in -o $out $libs\n  description = link $out\n\n",
        cc.join(" ")
    );
    for c in &plan.compiles {
        let flags: Vec<String> = c.flags.iter().map(|s| sh(s)).collect();
        let _ = writeln!(
            t,
            "build {}: cc {}\n  flags = {}",
            nj(&c.obj),
            nj(&c.src),
            flags.join(" ")
        );
    }
    let objs: Vec<String> = plan.compiles.iter().map(|c| nj(&c.obj)).collect();
    let libs: Vec<String> = plan.link_flags.iter().map(|s| sh(s)).collect();
    let _ = writeln!(
        t,
        "\nbuild {}: link {}\n  libs = {}\n\ndefault {}",
        nj(&plan.output),
        objs.join(" "),
        libs.join(" "),
        nj(&plan.output)
    );
    t
}

fn run(cmd: &mut std::process::Command, what: &str) -> Result<(), Error> {
    let out = cmd.output()?;
    if out.status.success() {
        return Ok(());
    }
    let mut msg = format!("{what} failed ({})", out.status);
    let tail = |b: &[u8]| {
        let s = String::from_utf8_lossy(b);
        let lines: Vec<&str> = s.lines().collect();
        lines[lines.len().saturating_sub(40)..].join("\n")
    };
    for b in [&out.stdout, &out.stderr] {
        let t = tail(b);
        if !t.is_empty() {
            msg.push('\n');
            msg.push_str(&t);
        }
    }
    Err(Error::Failed(msg))
}

fn newer(a: &Path, b: &Path) -> bool {
    let m = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
    match (m(a), m(b)) {
        (Some(x), Some(y)) => x > y,
        _ => true,
    }
}

/// Builds `plan` in `dir` with `jobs` parallel jobs (0: Ninja's default),
/// through Ninja at `ninja` when given, else directly.
///
/// # Errors
///
/// A write error, a tool that cannot start, or a failed compile or link
/// (with the end of its output).
pub fn build(plan: &Plan, dir: &Path, ninja: Option<&Path>, jobs: u32) -> Result<(), Error> {
    std::fs::create_dir_all(dir)?;
    for c in &plan.compiles {
        if let Some(p) = c.obj.parent() {
            std::fs::create_dir_all(p)?;
        }
    }
    if let Some(n) = ninja {
        relift_cache::write_if_changed(&dir.join("build.ninja"), ninja_file(plan).as_bytes())?;
        let mut cmd = relift_proc::command(n);
        cmd.current_dir(dir);
        if jobs > 0 {
            cmd.arg("-j").arg(jobs.to_string());
        }
        return run(&mut cmd, "ninja");
    }
    let (prog, pre) = plan
        .cc
        .split_first()
        .ok_or_else(|| Error::Failed("no C compiler".into()))?;
    let mut relink = !plan.output.exists();
    for c in &plan.compiles {
        if !newer(&c.src, &c.obj) {
            continue;
        }
        relink = true;
        let mut cmd = relift_proc::command(prog);
        cmd.args(pre)
            .args(&c.flags)
            .arg("-c")
            .arg(&c.src)
            .arg("-o")
            .arg(&c.obj);
        run(&mut cmd, &format!("cc {}", c.src.display()))?;
    }
    if relink {
        let mut cmd = relift_proc::command(prog);
        cmd.args(pre)
            .args(plan.compiles.iter().map(|c| &c.obj))
            .arg("-o")
            .arg(&plan.output)
            .args(&plan.link_flags);
        run(&mut cmd, "link")?;
    }
    Ok(())
}

/// Finds `name` on `PATH`.
pub fn find_tool(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .map(|d| d.join(name))
        .find(|p| p.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plan() -> Plan {
        Plan {
            cc: vec!["clang".into()],
            compiles: vec![Compile {
                src: "c/a b.c".into(),
                obj: "obj/a.o".into(),
                flags: vec!["-O2".into(), "-DX=\"y\"".into()],
            }],
            output: "game".into(),
            link_flags: vec!["-lm".into()],
        }
    }

    #[test]
    fn ninja_file_escapes_paths_and_quotes_flags() {
        let t = ninja_file(&plan());
        assert!(
            t.contains("build obj/a.o: cc c/a$ b.c\n  flags = -O2 '-DX=\"y\"'"),
            "{t}"
        );
        assert!(t.contains("build game: link obj/a.o\n  libs = -lm"), "{t}");
        assert!(t.contains("deps = gcc"));
    }

    #[test]
    fn shell_quoting() {
        assert_eq!(sh("plain-1.c"), "plain-1.c");
        assert_eq!(sh("it's"), "'it'\\''s'");
        assert_eq!(sh(""), "''");
    }
}
