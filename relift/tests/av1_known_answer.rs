//! The AV1 backend (dav1d, compiled from the pinned source the way `relift build` does) decodes a small stream
//! that was generated for this test (a synthetic test pattern, 64x48, six frames, encoded with libaom) to the
//! frames the dav1d command line tool, built with its assembly, writes for it. The stream's temporal units start
//! at the offsets below; the hashes are FNV-1a 64 over each frame's planes.
//!
//! The dav1d source comes from `RELIFT_DAV1D_DIR`, the test's cache or, failing both, `runtime/tools/dav1d_src.py`
//! (a download checked against its sha256). With no source and no network the test is skipped, or fails when
//! `RELIFT_DAV1D_REQUIRE` is set.

use relift::runtime_link::{Dav1d, Extras, find_dav1d};
use relift_build::Compile;
use std::path::{Path, PathBuf};

const UNITS: [u32; 6] = [0, 606, 648, 702, 1308, 1341];
const FRAME_HASHES: [&str; 6] = [
    "bd4dc4550f7342a0",
    "aedcfc23cf97f256",
    "f297b1336deb2573",
    "e9cfb7122f091505",
    "99c23845a2d76b8f",
    "99c23845a2d76b8f",
];

fn dav1d_source(workspace: &Path, cache: &Path) -> Option<Dav1d> {
    if let Some(d) = find_dav1d(cache) {
        return Some(d);
    }
    let tool = workspace.join("runtime/tools/dav1d_src.py");
    let ok = relift_proc::command("python3")
        .arg("-I")
        .arg(&tool)
        .arg("--cache")
        .arg(cache)
        .status()
        .is_ok_and(|s| s.success());
    ok.then(|| find_dav1d(cache)).flatten()
}

fn compile_all(units: Vec<Compile>) {
    let mut failed = Vec::new();
    for chunk in units.chunks(8) {
        let running: Vec<_> = chunk
            .iter()
            .map(|c| {
                std::fs::create_dir_all(c.obj.parent().unwrap()).unwrap();
                let child = relift_proc::command("clang")
                    .args(&c.flags)
                    .arg("-w")
                    .arg("-c")
                    .arg(&c.src)
                    .arg("-o")
                    .arg(&c.obj)
                    .spawn()
                    .unwrap();
                (c, child)
            })
            .collect();
        for (c, mut child) in running {
            if !child.wait().unwrap().success() {
                failed.push(c.src.clone());
            }
        }
    }
    assert!(failed.is_empty(), "clang failed on {failed:?}");
}

#[test]
fn the_av1_backend_decodes_a_generated_stream_to_the_reference_frames() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let workspace = manifest.parent().unwrap().to_path_buf();
    let work = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("av1-known-answer");
    let Some(dav1d) = dav1d_source(&workspace, &work.join("cache")) else {
        assert!(
            std::env::var_os("RELIFT_DAV1D_REQUIRE").is_none(),
            "no dav1d source and RELIFT_DAV1D_REQUIRE is set"
        );
        eprintln!("skipped: no dav1d source (set RELIFT_DAV1D_DIR or allow the download)");
        return;
    };
    let rt = workspace.join("runtime");
    let extras = Extras {
        vk: Vec::new(),
        window: None,
        sdl3: None,
        vulkan: None,
        host: Vec::new(),
        opus_src: None,
        opus: None,
        adrenotools: None,
        vpx: None,
        dav1d: Some(dav1d),
        dav1d_asm: None,
    };
    let own = vec![
        "-std=c11".to_string(),
        "-O1".into(),
        format!("-I{}", rt.join("core/include").display()),
        format!("-I{}", rt.join("core").display()),
    ];
    let obj_dir = work.join("obj");
    let obj = |s: &Path| {
        let rel = s.strip_prefix(&rt).unwrap_or(s);
        obj_dir.join(rel).with_extension("o")
    };
    let mut units = extras.compiles(&rt, &own, &obj);
    let flags: Vec<String> = own.iter().cloned().chain(extras.defines()).collect();
    for src in [
        rt.join("core/video.c"),
        manifest.join("tests/av1/known_answer.c"),
    ] {
        units.push(Compile {
            obj: obj_dir.join(src.file_name().unwrap()).with_extension("o"),
            src,
            flags: flags.clone(),
            pool: None,
            stage: 0,
        });
    }
    let objs: Vec<PathBuf> = units.iter().map(|c| c.obj.clone()).collect();
    compile_all(units);
    let exe = work.join("known_answer");
    let linked = relift_proc::command("clang")
        .args(&objs)
        .arg("-lpthread")
        .arg("-o")
        .arg(&exe)
        .status()
        .unwrap();
    assert!(linked.success(), "link failed");
    let out = relift_proc::command(&exe)
        .arg(manifest.join("tests/av1/stream.obu"))
        .args(UNITS.map(|u| u.to_string()))
        .output()
        .unwrap();
    assert!(out.status.success(), "decoder exited with {}", out.status);
    let got: Vec<String> = String::from_utf8(out.stdout)
        .unwrap()
        .lines()
        .map(str::to_string)
        .collect();
    assert_eq!(got, FRAME_HASHES);
}
