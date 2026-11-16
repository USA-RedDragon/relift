//! Whole romfs images: the image of a file list is the runtime's image of
//! the same tree, byte for byte; a map of a `.nup` serves the same bytes as
//! the tree the `.nup` extracts to, and checks its sources.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use relift_switch_formats::nup::{self, BuildPackage, Nup, PackageKind, UpdateSelect};
use relift_switch_formats::romfs::Romfs;
use relift_switch_formats::romfs_image::{self, Image, MAP_NAME, RomfsMap};
use relift_switch_formats::sha256;
use relift_switch_formats::storage::{FileStorage, MemStorage, read_all};

/// The SHA-256 the runtime's synthesizer gives the same tree
/// (`runtime/guests/switch/tests/test_fs_romfs.c`, `TREE_IMAGE_SHA256`).
const TREE_IMAGE_SHA256: &str = "aab39cbfc9b47d2af177964bdf0b8fd65ee678b118ee12e4404354277699829c";

const BIG_SIZE: usize = 1_100_000;

fn fill(n: usize, seed: u32) -> Vec<u8> {
    (0..n)
        .map(|i| {
            let i = i as u32;
            i.wrapping_mul(131)
                .wrapping_add((i >> 8).wrapping_mul(7))
                .wrapping_add(seed) as u8
        })
        .collect()
}

/// The runtime test's `make_tree`.
fn tree_files() -> (Vec<(String, Vec<u8>)>, Vec<String>) {
    let mut f = vec![
        ("a.txt".to_string(), b"hello, romfs\n".to_vec()),
        ("empty.bin".to_string(), Vec::new()),
        ("big.bin".to_string(), fill(BIG_SIZE, 3)),
        ("Zeta".to_string(), b"z".to_vec()),
        ("sub/deep/x.txt".to_string(), b"deep file".to_vec()),
    ];
    for i in 0..40 {
        f.push((format!("sub/n{i:02}"), fill(100 + i * 37, i as u32)));
    }
    let dirs = vec![
        "emptydir".to_string(),
        "sub".to_string(),
        "sub/deep".to_string(),
    ];
    (f, dirs)
}

fn image_bytes(files: &[(String, Vec<u8>)], dirs: &[String]) -> Vec<u8> {
    let list: Vec<(String, u64)> = files
        .iter()
        .map(|(p, b)| (p.clone(), b.len() as u64))
        .collect();
    let l = romfs_image::layout(&list, dirs).unwrap();
    let mut out = l.meta.clone();
    out.resize(l.size as usize, 0);
    for (i, (_, b)) in files.iter().enumerate() {
        let at = l.offsets[i] as usize;
        out[at..at + b.len()].copy_from_slice(b);
    }
    out
}

fn scratch(name: &str) -> PathBuf {
    let base = std::env::var_os("CARGO_TARGET_TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_TARGET_TMPDIR")));
    let d = base.join(format!("romfs-image-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn write_tree(root: &Path, files: &[(String, Vec<u8>)], dirs: &[String]) {
    for d in dirs {
        std::fs::create_dir_all(root.join(d)).unwrap();
    }
    for (p, b) in files {
        let path = root.join(p);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        relift_cache::write_if_changed(&path, b).unwrap();
    }
}

#[test]
fn the_layout_matches_the_runtimes_synthesized_image() {
    let (files, dirs) = tree_files();
    let img = image_bytes(&files, &dirs);
    assert_eq!(relift_switch_formats::hex(&sha256(&img)), TREE_IMAGE_SHA256);
    let r = Romfs::open(Arc::new(MemStorage(img))).unwrap();
    assert_eq!(r.files().len(), files.len());
    for (p, b) in &files {
        let f = r.files().iter().find(|f| &f.path == p).unwrap();
        let mut got = Vec::new();
        r.copy_file(f, &mut got).unwrap();
        assert_eq!(&got, b, "{p}");
    }
}

#[test]
fn a_tree_read_in_place_is_the_same_image() {
    let (files, dirs) = tree_files();
    let root = scratch("tree");
    write_tree(&root, &files, &dirs);
    let img = Image::from_tree(&root).unwrap();
    assert_eq!(read_all(&img).unwrap(), image_bytes(&files, &dirs));
    let opened = romfs_image::open(&root).unwrap();
    assert_eq!(
        relift_switch_formats::hex(&sha256(&read_all(&*opened).unwrap())),
        TREE_IMAGE_SHA256
    );
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_layout_refuses_clashing_names() {
    let f = |p: &str| (p.to_string(), 1u64);
    assert!(romfs_image::layout(&[f("a"), f("a/b")], &[]).is_err());
    assert!(romfs_image::layout(&[f("x/a"), f("x/a")], &[]).is_err());
    assert!(romfs_image::layout(&[f("x/../a")], &[]).is_err());
    assert!(romfs_image::layout(&[], &["d".to_string()]).is_ok());
}

fn nup_fixture() -> Vec<u8> {
    let (files, _) = tree_files();
    let paths: Vec<String> = files.iter().map(|(p, _)| format!("romfs/{p}")).collect();
    let mut list: Vec<(&str, &[u8])> = paths
        .iter()
        .zip(&files)
        .map(|(p, (_, b))| (p.as_str(), b.as_slice()))
        .collect();
    list.push(("exefs/main", b"main"));
    nup::build(&[BuildPackage {
        title_id: 0x0100_0000_0000_3000,
        version: 0,
        kind: PackageKind::Base,
        standalone: false,
        files: list,
        empty_dirs: vec!["romfs/emptydir"],
        meta: Vec::new(),
    }])
}

#[test]
fn a_nup_map_serves_the_extracted_trees_image() {
    let dir = scratch("nup");
    let nup_path = dir.join("game.nup");
    relift_cache::write_if_changed(&nup_path, &nup_fixture()).unwrap();
    let n = Nup::open(Arc::new(FileStorage::open(&nup_path).unwrap())).unwrap();
    let t = n.effective_title(UpdateSelect::Newest).unwrap();
    let out = dir.join("import");
    let w =
        romfs_image::write_nup(&n, &nup_path, &t.romfs, &t.romfs_dirs, &out, &mut |_| {}).unwrap();
    assert!(w.in_place > 0 && w.blob_files > 0, "{w:?}");
    let mapped = romfs_image::open(&out.join(MAP_NAME)).unwrap();
    assert_eq!(
        relift_switch_formats::hex(&sha256(&read_all(&*mapped).unwrap())),
        TREE_IMAGE_SHA256
    );
    let again =
        romfs_image::write_nup(&n, &nup_path, &t.romfs, &t.romfs_dirs, &out, &mut |_| {}).unwrap();
    assert_eq!(again.written, 0);

    let tree = dir.join("tree");
    n.extract_romfs(&t, &tree).unwrap();
    let from_tree = romfs_image::open(&tree).unwrap();
    assert_eq!(read_all(&*from_tree).unwrap(), read_all(&*mapped).unwrap());

    let map = RomfsMap::parse(&std::fs::read(out.join(MAP_NAME)).unwrap()).unwrap();
    assert_eq!(RomfsMap::parse(&map.to_bytes()).unwrap(), map);
    let blob = out.join(romfs_image::BLOB_NAME);
    let mut b = std::fs::read(&blob).unwrap();
    b[10] ^= 1;
    relift_cache::write_if_changed(&blob, &b).unwrap();
    assert!(romfs_image::open(&out.join(MAP_NAME)).is_err());
    b[10] ^= 1;
    b.push(0);
    relift_cache::write_if_changed(&blob, &b).unwrap();
    assert!(romfs_image::open(&out.join(MAP_NAME)).is_err());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_plain_image_file_opens_as_itself() {
    let (files, dirs) = tree_files();
    let dir = scratch("plain");
    let path = dir.join("romfs.img");
    relift_cache::write_if_changed(&path, &image_bytes(&files, &dirs)).unwrap();
    let s = romfs_image::open(&path).unwrap();
    assert_eq!(s.size(), std::fs::metadata(&path).unwrap().len());
    assert_eq!(Romfs::open(s).unwrap().files().len(), files.len());
    let _ = std::fs::remove_dir_all(&dir);
}
