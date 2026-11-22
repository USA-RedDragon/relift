//! Code mods end to end on the tiny program: a user mod changes a patched
//! function's output, a bundled mod is off and on in one binary (a code and
//! a data patch), and the build refuses the ambiguous cases. Skipped with a
//! note when clang is not on `PATH`.

mod tiny;

use std::path::Path;

/// `movz x1, #n`.
fn movz_x1(n: u32) -> [u8; 4] {
    (0xd280_0001 | (n << 5)).to_le_bytes()
}

/// An IPS32 writing `bytes` at image offset `at`.
fn ips32(records: &[(u32, &[u8])]) -> Vec<u8> {
    let mut p = b"IPS32".to_vec();
    for (at, bytes) in records {
        p.extend_from_slice(&(at + 0x100).to_be_bytes());
        p.extend_from_slice(&(bytes.len() as u16).to_be_bytes());
        p.extend_from_slice(bytes);
    }
    p.extend_from_slice(b"EEOF");
    p
}

fn put(path: &Path, bytes: &[u8]) {
    std::fs::create_dir_all(path.parent().expect("parent")).expect("dir");
    relift_cache::write_if_changed(path, bytes).expect("write");
}

/// The patch file name for the tiny program's (all-zero) build id.
const ID: &str = "0000000000000000";

struct Project {
    root: std::path::PathBuf,
    dir: std::path::PathBuf,
    cache: String,
}

impl Project {
    fn new(name: &str) -> Option<Project> {
        if relift_build::find_tool("clang").is_none() {
            eprintln!("skipped: no clang on PATH");
            return None;
        }
        let root = tiny::scratch(name);
        let dir = tiny::dump(&root);
        let cache = root.join("cache").display().to_string();
        let out = tiny::relift(&dir, &["--cache-dir", &cache, "import", "../dump"]);
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        Some(Project { root, dir, cache })
    }

    fn build(&self, extra: &[&str]) -> std::process::Output {
        let mut args = vec!["--cache-dir", &self.cache, "-j", "4", "build"];
        args.extend_from_slice(extra);
        tiny::relift(&self.dir, &args)
    }

    fn run(&self, args: &[&str]) -> String {
        let exe = self.root.join("cache/projects/tiny/build/host/tiny");
        let out = relift_proc::command(&exe)
            .args(args)
            .current_dir(&self.dir)
            .output()
            .expect("the program runs");
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        String::from_utf8_lossy(&out.stdout).into_owned()
    }

    fn add_game_toml(&self, text: &str) {
        let p = self.dir.join("game.toml");
        let mut t = std::fs::read_to_string(&p).expect("game.toml");
        t.push_str(text);
        relift_cache::write_if_changed(&p, t.as_bytes()).expect("write");
    }
}

fn ok(out: &std::process::Output) -> String {
    let e = String::from_utf8_lossy(&out.stderr).into_owned();
    assert!(out.status.success(), "{e}");
    e
}

fn refused(out: &std::process::Output) -> String {
    assert!(!out.status.success(), "the build should be refused");
    String::from_utf8_lossy(&out.stderr).into_owned()
}

#[test]
fn a_user_mod_changes_the_patched_function_and_lifts_only_it() {
    let Some(p) = Project::new("mods-e2e-user") else {
        return;
    };
    ok(&p.build(&[]));
    assert_eq!(p.run(&[]), "hello from relift\n".repeat(3));
    let m = p.root.join("short");
    put(
        &m.join(format!("exefs_patches/short/{ID}.ips")),
        &ips32(&[(0x30, &movz_x1(5))]),
    );
    let e = ok(&p.build(&["--mods", m.to_str().expect("utf-8")]));
    assert!(
        e.contains("main: code mods changed 1 function; 1 reused"),
        "{e}"
    );
    assert_eq!(p.run(&[]), "hello\n".repeat(3));
    let info: serde_json::Value =
        serde_json::from_str(&p.run(&["--relift-info"])).expect("--relift-info prints JSON");
    assert_eq!(info["mods"]["code"][0]["name"], "short", "{info}");
    let other = p.root.join("other");
    put(
        &other.join(format!("exefs_patches/x/{ID}.ips")),
        &ips32(&[(0x30, &movz_x1(4))]),
    );
    let both = format!("{},{}", m.display(), other.display());
    let e = refused(&p.build(&["--mods", &both]));
    assert!(e.contains("short and other write different bytes"), "{e}");
}

#[test]
fn a_bundled_mod_is_switched_at_start() {
    let Some(p) = Project::new("mods-e2e-bundled") else {
        return;
    };
    put(
        &p.dir
            .join(format!("mods/short/exefs_patches/short/{ID}.ips")),
        &ips32(&[(0x30, &movz_x1(5)), (0x1000, b"j")]),
    );
    p.add_game_toml(
        "\n[[mods.bundled]]\nname = \"short\"\nlabel = \"Short\"\npatch = \"mods/short\"\n",
    );
    ok(&p.build(&[]));
    assert_eq!(p.run(&[]), "hello from relift\n".repeat(3));
    assert_eq!(
        p.run(&["--relift-set", "mod.short=true"]),
        "jello\n".repeat(3)
    );
    let user = p.root.join(format!("{ID}.ips"));
    put(&user, &ips32(&[(0x30, &movz_x1(3))]));
    let e = refused(&p.build(&["--mods", user.to_str().expect("utf-8")]));
    assert!(
        e.contains(&format!("{ID}.ips patches main"))
            && e.contains("which the bundled mod short also patches"),
        "{e}"
    );
    put(
        &p.dir.join(format!("mods/long/{ID}.ips")),
        &ips32(&[(0x30, &movz_x1(9))]),
    );
    p.add_game_toml("\n[[mods.bundled]]\nname = \"long\"\npatch = \"mods/long\"\n");
    let e = refused(&p.build(&[]));
    assert!(
        e.contains("the bundled mods short and long both patch main"),
        "{e}"
    );
    p.add_game_toml("exclusive = [\"short\"]\n");
    ok(&p.build(&[]));
    assert_eq!(
        p.run(&["--relift-set", "mod.long=true"]),
        "hello fro\n".repeat(3)
    );
}

#[test]
fn the_stored_mods_section_lists_code_bundled_and_accepted_mods() {
    let Some(p) = Project::new("mods-e2e-section") else {
        return;
    };
    put(
        &p.dir
            .join(format!("mods/short/exefs_patches/short/{ID}.ips")),
        &ips32(&[(0x30, &movz_x1(5))]),
    );
    p.add_game_toml(
        "\n[[mods.bundled]]\nname = \"short\"\nlabel = \"Short\"\npatch = \"mods/short\"\n",
    );
    let user = p.root.join("user");
    put(
        &user.join(format!("exefs_patches/user/{ID}.ips")),
        &ips32(&[(0x40, b"j")]),
    );
    ok(&p.build(&["--mods", user.to_str().expect("utf-8")]));
    let data = p.root.join("data");
    put(
        &data.join("atmosphere/contents/0100000000000000/romfs/a.txt"),
        b"a",
    );
    let info: serde_json::Value =
        serde_json::from_str(&p.run(&["--relift-info"])).expect("--relift-info prints JSON");
    let mods = &info["mods"];
    assert_eq!(mods["code"][0]["name"], "user", "{info}");
    assert_eq!(mods["bundled"][0], "mod.short", "{info}");
    assert!(
        !mods["accepts"].as_array().expect("accepts").is_empty(),
        "{info}"
    );
    let keys: Vec<&str> = info["settings"]["game"]
        .as_array()
        .expect("game settings")
        .iter()
        .filter_map(|s| s["key"].as_str())
        .collect();
    assert!(keys.contains(&"mod.short"), "{keys:?}");
}
