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
    let mut nacp = vec![0u8; 0x4000];
    for (slot, name) in [(0usize, "Tiny Game"), (3, "Petit Jeu")] {
        nacp[slot * 0x300..slot * 0x300 + name.len()].copy_from_slice(name.as_bytes());
    }
    relift_cache::write_if_changed(&root.join("dump/control.nacp"), &nacp).unwrap();
    relift_cache::write_if_changed(&root.join("dump/icon.jpg"), include_bytes!("icon.jpg"))
        .unwrap();
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
        .map(|e| {
            let e = e.unwrap();
            e.path().join("build/host").join(format!(
                "{}{}",
                e.file_name().to_string_lossy(),
                std::env::consts::EXE_SUFFIX
            ))
        })
        .find(|p| p.is_file())
        .expect("the built program");
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
    assert_eq!(info.identity.title(&["French"]), Some("Petit Jeu"));
    assert_eq!(info.identity.title(&[]), Some("Tiny Game"));
    let icon = info.identity.icon.as_ref().expect("the control icon");
    assert_eq!(
        (icon.format.as_str(), icon.width, icon.height),
        ("png", 16, 8)
    );
    let at = |hay: &[u8]| {
        hay.windows(icon.data.len())
            .filter(|w| *w == icon.data.as_bytes())
            .count()
    };
    assert_eq!(at(&image), 1, "the icon is stored once in the program");
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
