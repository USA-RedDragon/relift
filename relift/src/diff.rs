//! `relift diff`: one lifted function against the reference interpreter in a
//! live run (docs/abi.md, "Differential checks").
//!
//! The program runs under gdb with `runtime/tools/gdb/relift_diff.py`, which
//! stops at each call of the function and talks to an engine, by default
//! `relift diff --serve` ([`serve`]), over a pipe.

use std::path::{Path, PathBuf};

use relift_diff::{Wire, serve as serve_wire};
use relift_names::fallback_name;
use relift_switch::program::c_module;

use crate::build::{Mode, build_project};
use crate::diff_ir::IrEngine;
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// What `relift diff` was asked.
#[derive(Debug)]
pub struct Request<'a> {
    /// The function to check.
    pub function: Option<&'a str>,
    /// How many calls to compare.
    pub count: u32,
    /// How many calls to let pass first.
    pub skip: u32,
    /// The engine command line, instead of this program's `--serve`.
    pub engine: Option<&'a str>,
    /// Be the engine, on standard input and output.
    pub serve: bool,
    /// Flip a bit of this memory read of the interpreter run (0: none).
    pub flip_read: u64,
}

/// The host name of the function `what` names: a host name as it is, or
/// `module:symbol`, or `module+0xoffset`.
///
/// # Errors
///
/// A spelling that is none of the three, or a symbol the host-name scheme
/// cannot spell.
pub fn host_name(what: &str) -> Result<String, BoxError> {
    if what.starts_with("_ZN5guest") {
        return Ok(what.to_string());
    }
    if let Some((module, symbol)) = what.split_once(':') {
        return relift_names::host_name(&c_module(module), symbol)
            .ok_or_else(|| format!("the host-name scheme cannot spell `{symbol}`").into());
    }
    if let Some((module, off)) = what.split_once('+') {
        let off = u64::from_str_radix(off.trim_start_matches("0x"), 16)
            .map_err(|e| format!("`{off}` is not a hex offset: {e}"))?;
        return Ok(fallback_name(&c_module(module), off));
    }
    Err(format!("`{what}` is not a host name, module:symbol or module+0xoffset").into())
}

/// Serves the protocol on standard input and output with the interpreter
/// engine until the driver closes the pipe.
///
/// # Errors
///
/// A broken pipe or a line out of the protocol.
pub fn serve(settings: &Settings, flip_read: u64) -> Result<(), BoxError> {
    let mut wire = Wire::stdio();
    let cpu = crate::build::project_cpu(settings)?;
    let mut engine = IrEngine::new(cpu).flip_read(flip_read);
    serve_wire(&mut wire, &mut engine, &mut std::io::stderr())?;
    Ok(())
}

fn script(rt: &Path) -> Result<PathBuf, BoxError> {
    let p = rt.join("tools/gdb/relift_diff.py");
    if p.is_file() {
        Ok(p)
    } else {
        Err(format!("{} is missing", p.display()).into())
    }
}

/// `relift diff`: the exit code.
///
/// # Errors
///
/// A bad function spelling, a failed build, no gdb, or a program that cannot
/// start.
pub fn run(settings: &Settings, req: &Request<'_>, ui: &mut Ui) -> Result<u8, BoxError> {
    if req.serve {
        serve(settings, req.flip_read)?;
        return Ok(0);
    }
    let what = req.function.ok_or("a function is needed (or --serve)")?;
    let host = host_name(what)?;
    let gdb = relift_build::find_tool("gdb").ok_or("gdb is not on PATH")?;
    let built = build_project(settings, &[], &[], Mode::Dev, ui)?;
    let rt = if settings.runtime_dir.as_os_str().is_empty() {
        PathBuf::from(crate::build::RUNTIME_DIR)
    } else {
        settings.runtime_dir.clone()
    };
    let engine = match req.engine {
        Some(e) => e.to_string(),
        None => {
            let me = std::env::current_exe()?;
            let mut s = format!("{} diff --serve", shell_word(&me.display().to_string()));
            if req.flip_read > 0 {
                s.push_str(&format!(" --flip-read {}", req.flip_read));
            }
            s
        }
    };
    let import = crate::build::import_dir(settings)?;
    let mut cmd = relift_proc::command(&gdb);
    cmd.args(["-q", "-batch", "-nx", "-iex", "set debuginfod enabled off"])
        .arg("-ex")
        .arg(format!("source {}", script(&rt)?.display()))
        .args(["-ex", "relift-diff-start"])
        .arg("-ex")
        .arg(format!("relift-diff {host} {} {}", req.count, req.skip))
        .args(["-ex", "relift-diff-finish"])
        .arg("--args")
        .arg(&built.exe)
        .env("RELIFT_DIFF_ENGINE", engine);
    crate::gamedata::set_env(&mut cmd, &import);
    crate::shaders::set_env(&mut cmd, &built.out);
    let status = cmd.status()?;
    Ok(u8::try_from(status.code().unwrap_or(128)).unwrap_or(1))
}

fn shell_word(s: &str) -> String {
    if s.chars()
        .all(|c| c.is_ascii_alphanumeric() || "/._-+".contains(c))
    {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_three_spellings_resolve() {
        assert_eq!(
            host_name("_ZN5guest4main3fooEv").unwrap(),
            "_ZN5guest4main3fooEv"
        );
        assert_eq!(
            host_name("main+0x30").unwrap(),
            "_ZN5guest4main11fn_00000030E"
        );
        assert_eq!(
            host_name("my-mod:_Z3foov").unwrap(),
            "_ZN5guest6my_mod3fooEv"
        );
        assert!(host_name("nonsense").is_err());
        assert!(host_name("main+0xzz").is_err());
    }
}
