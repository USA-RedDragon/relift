//! What every test that translates shares: `translate` and `translate_with`
//! that also run each module they produce through `spirv-val` (Vulkan 1.2).
//! Without `spirv-val` on the path the validation is skipped, with one
//! message; set `RELIFT_SPIRV_VAL_REQUIRE` to make that a failure instead.

use relift_gpu_nv::program::Program;
use relift_gpu_nv::translate::{self, Options, Shader, Unsupported};
use std::io::Write;
use std::process::Stdio;
use std::sync::Once;

static SKIP: Once = Once::new();

/// Runs `spirv-val` on a module; panics with its message if it is invalid.
pub fn validate(words: &[u32]) {
    let child = relift_proc::command("spirv-val")
        .args(["--target-env", "vulkan1.2", "-"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn();
    let mut child = match child {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            assert!(
                std::env::var_os("RELIFT_SPIRV_VAL_REQUIRE").is_none(),
                "spirv-val not found and RELIFT_SPIRV_VAL_REQUIRE is set"
            );
            SKIP.call_once(|| eprintln!("spirv-val not found: SPIR-V validation skipped"));
            return;
        }
        Err(e) => panic!("spirv-val: {e}"),
    };
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    child.stdin.take().unwrap().write_all(&bytes).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(
        out.status.success(),
        "spirv-val rejects the module:\n{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
}

pub fn translate(p: &Program) -> Result<Shader, Unsupported> {
    let s = translate::translate(p)?;
    validate(&s.spirv);
    Ok(s)
}

pub fn translate_with(p: &Program, o: &Options) -> Result<Shader, Unsupported> {
    let s = translate::translate_with(p, o)?;
    validate(&s.spirv);
    Ok(s)
}

/// Keeps both wrappers in use in every test binary, and checks the
/// smallest program validates.
#[test]
fn an_empty_compute_program_validates() {
    let mut code = Vec::new();
    let exit = 0x14du128 | (4 << 9) | (7 << 12) | (7 << 87) | (7 << 110) | (7 << 113);
    let bra = 0x147u128 | (4 << 9) | (7 << 12) | (0xffff_ffff_fffc << 34) | (7 << 87);
    code.extend(exit.to_le_bytes());
    code.extend(bra.to_le_bytes());
    let p = relift_gpu_sm86::program::parse(&code);
    translate(&p).unwrap_or_else(|e| panic!("{e}"));
    translate_with(&p, &Options::default()).unwrap_or_else(|e| panic!("{e}"));
}
