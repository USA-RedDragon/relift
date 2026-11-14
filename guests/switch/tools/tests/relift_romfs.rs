//! `relift-romfs` against a synthetic romfs image.

mod common;

use common::*;

fn image(name: &str) -> (Fixture, std::path::PathBuf) {
    let f = fixture(name);
    let img = f.romfs.clone();
    (f, img)
}

#[test]
fn ls_lists_paths_and_sizes() {
    let (f, img) = image("romfs-ls");
    let v = json(&run("relift-romfs", &f.dir, &["ls", path(&img), "--json"]));
    let got: Vec<(String, usize)> = v
        .as_array()
        .expect("array")
        .iter()
        .map(|e| {
            (
                e["path"].as_str().expect("path").to_string(),
                usize::try_from(e["size"].as_u64().expect("size")).expect("fits"),
            )
        })
        .collect();
    let want: Vec<(String, usize)> = sorted(&f.game.base_spec.romfs)
        .into_iter()
        .map(|(p, d)| (p, d.len()))
        .collect();
    assert_eq!(got, want);
    let text = run("relift-romfs", &f.dir, &["ls", path(&img)]);
    assert!(
        String::from_utf8_lossy(&ok(&text).stdout).contains("readme.txt\t9"),
        "text listing"
    );
}

#[test]
fn extract_writes_the_tree_and_cat_one_file() {
    let (f, img) = image("romfs-extract");
    let out = f.dir.join("tree");
    ok(&run(
        "relift-romfs",
        &f.dir,
        &["extract", path(&img), "-o", path(&out)],
    ));
    assert_eq!(tree(&out), sorted(&f.game.base_spec.romfs));
    let cat = run(
        "relift-romfs",
        &f.dir,
        &["cat", path(&img), "data/keep.txt"],
    );
    assert_eq!(ok(&cat).stdout, b"kept");
    let cat = run("relift-romfs", &f.dir, &["cat", path(&img), "/readme.txt"]);
    assert_eq!(ok(&cat).stdout, b"version 0");
}

#[test]
fn bad_inputs_exit_one_with_one_line() {
    let (f, img) = image("romfs-bad");
    fails_with(
        &run("relift-romfs", &f.dir, &["cat", path(&img), "nope.txt"]),
        "no such file",
    );
    fails_with(
        &run("relift-romfs", &f.dir, &["ls", "missing.img"]),
        "missing.img",
    );
    let out = run_stdin("relift-romfs", &f.dir, &["ls", "-"], &[0u8; 64]);
    assert_eq!(out.status.code(), Some(1));
}
