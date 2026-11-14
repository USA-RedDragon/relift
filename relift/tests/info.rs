//! The self-description end to end: a tiny game is built, the document is
//! read from the binary's section without running it, and `--relift-info`
//! prints the same bytes.

mod tiny;

use relift_info::{Info, section};

#[test]
fn the_section_and_the_flag_give_the_same_document() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("info-e2e");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let exe = std::fs::read_dir(root.join("cache").join("projects"))
        .unwrap()
        .map(|e| e.unwrap().path().join("build/host/game"))
        .find(|p| p.is_file())
        .expect("the built game");
    let image = std::fs::read(&exe).unwrap();
    let from_section = section::read(&image).expect("a section").to_vec();
    let run = relift_proc::command(&exe)
        .arg("--relift-info")
        .output()
        .unwrap();
    assert!(run.status.success());
    assert_eq!(run.stdout, from_section);
    let info = Info::from_bytes(&from_section).unwrap();
    assert_eq!(info.relift.protocol, relift_info::PROTOCOL);
    assert_eq!(info.identity.platform, "switch");
    assert_eq!(info.identity.title_id.as_deref(), Some("0100000000001234"));
    assert_eq!(info.identity.modules.len(), 1);
    assert_eq!(info.mods.accepts, relift_info::Mods::switch_accepts());
    let cli = tiny::relift(&project, &["--cache-dir", &cache, "info"]);
    assert!(
        cli.status.success(),
        "{}",
        String::from_utf8_lossy(&cli.stderr)
    );
    assert_eq!(cli.stdout, from_section);
    let named = tiny::relift(&project, &["info", exe.to_str().unwrap()]);
    assert_eq!(named.stdout, from_section);
}
